// Copyright (C) 2020-2022 Free Software Foundation, Inc.

// This file is part of GCC.

// GCC is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any later
// version.

// GCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

// You should have received a copy of the GNU General Public License
// along with GCC; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

#include "rust-hir-type-check.h"
#include "rust-hir-full.h"
#include "rust-hir-type-check-expr.h"
#include "rust-hir-type-check-struct-field.h"

namespace Rust {
namespace Resolver {

void
TypeCheckStructExpr::visit (HIR::StructExprStructFields &struct_expr)
{
  TyTy::BaseType *struct_path_ty
    = TypeCheckExpr::Resolve (&struct_expr.get_struct_name (), false);
  if (struct_path_ty->get_kind () != TyTy::TypeKind::ADT)
    {
      rust_error_at (struct_expr.get_struct_name ().get_locus (),
		     "expected an ADT type for constructor");
      return;
    }

  struct_path_resolved = static_cast<TyTy::ADTType *> (struct_path_ty);
  TyTy::ADTType *struct_def = struct_path_resolved;
  if (struct_expr.has_struct_base ())
    {
      TyTy::BaseType *base_resolved
	= TypeCheckExpr::Resolve (struct_expr.struct_base->base_struct.get (),
				  false);
      struct_def
	= (TyTy::ADTType *) struct_path_resolved->unify (base_resolved);
      if (struct_def == nullptr)
	{
	  rust_fatal_error (struct_expr.struct_base->base_struct->get_locus (),
			    "incompatible types for base struct reference");
	  return;
	}
    }

  // figure out the variant
  if (struct_path_resolved->is_enum ())
    {
      // lookup variant id
      HirId variant_id;
      bool ok = context->lookup_variant_definition (
	struct_expr.get_struct_name ().get_mappings ().get_hirid (),
	&variant_id);
      rust_assert (ok);

      ok = struct_path_resolved->lookup_variant_by_id (variant_id, &variant);
      rust_assert (ok);
    }
  else
    {
      rust_assert (struct_path_resolved->number_of_variants () == 1);
      variant = struct_path_resolved->get_variants ().at (0);
    }

  std::vector<TyTy::StructFieldType *> infered_fields;
  bool ok = true;

  for (auto &field : struct_expr.get_fields ())
    {
      resolved_field_value_expr = nullptr;
      field->accept_vis (*this);
      if (resolved_field_value_expr == nullptr)
	{
	  rust_fatal_error (field->get_locus (),
			    "failed to resolve type for field");
	  ok = false;
	  break;
	}

      context->insert_type (field->get_mappings (), resolved_field_value_expr);
    }

  // something failed setting up the fields
  if (!ok)
    {
      rust_error_at (struct_expr.get_locus (),
		     "constructor type resolution failure");
      return;
    }

  // check the arguments are all assigned and fix up the ordering
  if (fields_assigned.size () != variant->num_fields ())
    {
      if (struct_def->is_union ())
	{
	  if (fields_assigned.size () != 1 || struct_expr.has_struct_base ())
	    {
	      rust_error_at (
		struct_expr.get_locus (),
		"union must have exactly one field variant assigned");
	      return;
	    }
	}
      else if (!struct_expr.has_struct_base ())
	{
	  rust_error_at (struct_expr.get_locus (),
			 "constructor is missing fields");
	  return;
	}
      else
	{
	  // we have a struct base to assign the missing fields from.
	  // the missing fields can be implicit FieldAccessExprs for the value
	  std::set<std::string> missing_fields;
	  for (auto &field : variant->get_fields ())
	    {
	      auto it = fields_assigned.find (field->get_name ());
	      if (it == fields_assigned.end ())
		missing_fields.insert (field->get_name ());
	    }

	  // we can generate FieldAccessExpr or TupleAccessExpr for the
	  // values of the missing fields.
	  for (auto &missing : missing_fields)
	    {
	      HIR::Expr *receiver
		= struct_expr.struct_base->base_struct->clone_expr_impl ();

	      HIR::StructExprField *implicit_field = nullptr;

	      AST::AttrVec outer_attribs;
	      auto crate_num = mappings->get_current_crate ();
	      Analysis::NodeMapping mapping (
		crate_num,
		struct_expr.struct_base->base_struct->get_mappings ()
		  .get_nodeid (),
		mappings->get_next_hir_id (crate_num), UNKNOWN_LOCAL_DEFID);

	      HIR::Expr *field_value = new HIR::FieldAccessExpr (
		mapping, std::unique_ptr<HIR::Expr> (receiver), missing,
		std::move (outer_attribs),
		struct_expr.struct_base->base_struct->get_locus ());

	      implicit_field = new HIR::StructExprFieldIdentifierValue (
		mapping, missing, std::unique_ptr<HIR::Expr> (field_value),
		struct_expr.struct_base->base_struct->get_locus ());

	      size_t field_index;
	      bool ok = variant->lookup_field (missing, nullptr, &field_index);
	      rust_assert (ok);

	      adtFieldIndexToField[field_index] = implicit_field;
	      struct_expr.get_fields ().push_back (
		std::unique_ptr<HIR::StructExprField> (implicit_field));
	    }
	}
    }

  if (struct_def->is_union ())
    {
      // There is exactly one field in this constructor, we need to
      // figure out the field index to make sure we initialize the
      // right union field.
      for (size_t i = 0; i < adtFieldIndexToField.size (); i++)
	{
	  if (adtFieldIndexToField[i])
	    {
	      struct_expr.union_index = i;
	      break;
	    }
	}
      rust_assert (struct_expr.union_index != -1);
    }
  else
    {
      // everything is ok, now we need to ensure all field values are ordered
      // correctly. The GIMPLE backend uses a simple algorithm that assumes each
      // assigned field in the constructor is in the same order as the field in
      // the type
      for (auto &field : struct_expr.get_fields ())
	field.release ();

      std::vector<std::unique_ptr<HIR::StructExprField> > ordered_fields;
      for (size_t i = 0; i < adtFieldIndexToField.size (); i++)
	{
	  ordered_fields.push_back (
	    std::unique_ptr<HIR::StructExprField> (adtFieldIndexToField[i]));
	}
      struct_expr.set_fields_as_owner (std::move (ordered_fields));
    }

  resolved = struct_def;
}

void
TypeCheckStructExpr::visit (HIR::StructExprFieldIdentifierValue &field)
{
  auto it = fields_assigned.find (field.field_name);
  if (it != fields_assigned.end ())
    {
      rust_fatal_error (field.get_locus (), "used more than once");
      return;
    }

  size_t field_index;
  TyTy::StructFieldType *field_type;
  bool ok = variant->lookup_field (field.field_name, &field_type, &field_index);
  if (!ok)
    {
      rust_error_at (field.get_locus (), "unknown field");
      return;
    }

  TyTy::BaseType *value = TypeCheckExpr::Resolve (field.get_value (), false);
  resolved_field_value_expr = field_type->get_field_type ()->unify (value);
  if (resolved_field_value_expr != nullptr)
    {
      fields_assigned.insert (field.field_name);
      adtFieldIndexToField[field_index] = &field;
    }
}

void
TypeCheckStructExpr::visit (HIR::StructExprFieldIndexValue &field)
{
  std::string field_name (std::to_string (field.get_tuple_index ()));
  auto it = fields_assigned.find (field_name);
  if (it != fields_assigned.end ())
    {
      rust_fatal_error (field.get_locus (), "used more than once");
      return;
    }

  size_t field_index;
  TyTy::StructFieldType *field_type;
  bool ok = variant->lookup_field (field_name, &field_type, &field_index);
  if (!ok)
    {
      rust_error_at (field.get_locus (), "unknown field");
      return;
    }

  TyTy::BaseType *value = TypeCheckExpr::Resolve (field.get_value (), false);
  resolved_field_value_expr = field_type->get_field_type ()->unify (value);
  if (resolved_field_value_expr != nullptr)
    {
      fields_assigned.insert (field_name);
      adtFieldIndexToField[field_index] = &field;
    }
}

void
TypeCheckStructExpr::visit (HIR::StructExprFieldIdentifier &field)
{
  auto it = fields_assigned.find (field.get_field_name ());
  if (it != fields_assigned.end ())
    {
      rust_fatal_error (field.get_locus (), "used more than once");
      return;
    }

  size_t field_index;
  TyTy::StructFieldType *field_type;
  bool ok = variant->lookup_field (field.get_field_name (), &field_type,
				   &field_index);
  if (!ok)
    {
      rust_error_at (field.get_locus (), "unknown field");
      return;
    }

  // we can make the field look like an identifier expr to take advantage of
  // existing code to figure out the type
  HIR::IdentifierExpr expr (field.get_mappings (), field.get_field_name (),
			    field.get_locus ());
  TyTy::BaseType *value = TypeCheckExpr::Resolve (&expr, false);

  resolved_field_value_expr = field_type->get_field_type ()->unify (value);
  if (resolved_field_value_expr != nullptr)

    {
      fields_assigned.insert (field.field_name);
      adtFieldIndexToField[field_index] = &field;
    }
}

} // namespace Resolver
} // namespace Rust
