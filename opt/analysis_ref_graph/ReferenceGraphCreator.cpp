/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ReferenceGraphCreator.h"

#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "Walkers.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "Resolver.h"

void CreateReferenceGraphPass::build_super_and_interface_refs(
    const Scope& scope,
    refs_t& class_refs) {
  for (const auto* child : scope) {
    std::function<void(const DexType*)> recurse =
      [&class_refs, &child, &recurse] (const DexType* super) {
        if (super != nullptr) {
          class_refs[child].emplace(super);
          const auto super_cls_or_int = type_class(super);
          if (super_cls_or_int != nullptr) {
            recurse(super_cls_or_int->get_super_class());
            for (const auto* interface : super_cls_or_int->get_interfaces()->get_type_list()) {
              recurse(interface);
            }
          }
        }
    };
    recurse(child->get_type());
  }
}

template <class T>
void CreateReferenceGraphPass::get_annots(
    const T* thing_with_annots,
    const DexClass* enclosing_class,
    refs_t& class_refs) {
  const auto& thing_anno_set = thing_with_annots->get_anno_set();
  if (thing_anno_set != nullptr) {
    for (const auto* annot : thing_anno_set->get_annotations()) {
      class_refs[enclosing_class].emplace(annot->type());
    }
  }
}

CreateReferenceGraphPass::MethodWalkerFn
CreateReferenceGraphPass::method_ref_builder(
    const Scope& scope,
    refs_t& class_refs) {
  return ([this, &scope, &class_refs](const DexMethod* method) {
    const auto* enclosing_class = type_class(method->get_class());
    // do not add annotations to a method call. Only to method definition
    get_annots(method, enclosing_class, class_refs);

    std::vector<DexType*> types;
    method->get_proto()->gather_types(types);
    for (const auto* t : types) {
      if (t) class_refs[enclosing_class].emplace(t);
    }
  });
}

CreateReferenceGraphPass::FieldWalkerFn
CreateReferenceGraphPass::field_ref_builder(
    const Scope& scope,
    refs_t& class_refs) {
  return ([this, &scope, &class_refs](DexField* field) {
    const auto* enclosing_class = type_class(field->get_class());

    const DexField* field_maybe_resolved;
    if (config.resolve_fields) {
      field_maybe_resolved = resolve_field(field);
    } else {
      field_maybe_resolved = field;
    }
    get_annots(field, enclosing_class, class_refs);
    const auto* t = field->get_type();
    if (t) class_refs[enclosing_class].emplace(t);
  });
}

void CreateReferenceGraphPass::build_class_annot_refs(
    const Scope& scope,
    refs_t& class_refs) {
  for (const auto* cls : scope) {
    get_annots(cls, cls, class_refs);
  }
}

CreateReferenceGraphPass::MethodWalkerFn
CreateReferenceGraphPass::method_annot_ref_builder(
    const Scope& scope,
    refs_t& class_refs) {
  return ([this, &scope, &class_refs](const DexMethod* meth) {
   for (const auto* cls : scope) {
     get_annots(meth, cls, class_refs);
    }
  });
}

CreateReferenceGraphPass::FieldWalkerFn
CreateReferenceGraphPass::field_annot_ref_builder(
    const Scope& scope,
    refs_t& class_refs) {
  return ([this, &scope, &class_refs](const DexField* field) {
    for (const auto* cls : scope) {
     get_annots(field, cls, class_refs);
    }
  });
}

CreateReferenceGraphPass::MethodWalkerFn
CreateReferenceGraphPass::exception_ref_builder(
    const Scope& scope,
    refs_t& class_refs) {
  return ([this, &scope, &class_refs](const DexMethod* meth) {
    const auto* enclosing_class = type_class(meth->get_class());

    for (const auto& tries : meth->get_code()->get_tries()) {
      for (const auto& catch_pair : tries->m_catches) {
        const auto* caught_except = catch_pair.first;
        if (caught_except) class_refs[enclosing_class].emplace(caught_except);
      }
    }
  });
}

CreateReferenceGraphPass::InstructionWalkerFn
CreateReferenceGraphPass::instruction_ref_builder(
    const Scope& scope,
    refs_t& class_refs) {
  return ([this, &scope, &class_refs](const DexMethod* meth, DexInstruction* insn) {
    const auto* enclosing_class = type_class(meth->get_class());

    if (insn->has_types()) {
      const auto* top = static_cast<DexOpcodeType*>(insn);
      const auto* tref = top->get_type();
      if (tref) class_refs[enclosing_class].emplace(tref);
      return;
    }
    if (insn->has_fields()) {
      const auto* top = static_cast<DexOpcodeField*>(insn);
      auto* field = top->field();
      if (CreateReferenceGraphPass::config.resolve_fields) {
        field = resolve_field(field);
      }
      const auto* field_owner = field->get_class();
      const auto* field_type = field->get_type();
      if (field_owner) class_refs[enclosing_class].emplace(field_owner);
      if (field_type) class_refs[enclosing_class].emplace(field_type);
      return;
    }
    if (insn->has_methods()) {
      const auto* mop = static_cast<DexOpcodeMethod*>(insn);
      auto* method = mop->get_method();
      if (CreateReferenceGraphPass::config.resolve_methods) {
        method = resolve_method(method, MethodSearch::Any);
      }

      // argument and return types
      std::vector<DexType*> types;
      method->get_proto()->gather_types(types);
      for (const auto* t : types) {
        if (t) class_refs[enclosing_class].emplace(t);
      }

      return;
    }
  });
}

void CreateReferenceGraphPass::gather_all(const Scope& scope, refs_t& class_refs) {
  for (const auto* cls : scope) {
    std::vector<DexType*> types;
    cls->gather_types(types);
    for (const auto* t : types) {
      if (t) class_refs[cls].emplace(t);
    }
  }
}

void CreateReferenceGraphPass::build_refs(const Scope& scope, refs_t& class_refs) {
  if (CreateReferenceGraphPass::config.gather_all) {
    gather_all(scope, class_refs);
  } else {
    if (CreateReferenceGraphPass::config.refs_in_annotations) {
      build_class_annot_refs(scope, class_refs);
      walk_methods(scope, method_annot_ref_builder(scope, class_refs));
      walk_fields(scope, field_annot_ref_builder(scope, class_refs));
    }
    if (CreateReferenceGraphPass::config.refs_in_class_structure) {
      build_super_and_interface_refs(scope, class_refs);
      walk_methods(scope, method_ref_builder(scope, class_refs));
      walk_fields(scope, field_ref_builder(scope, class_refs));
    }
    if (CreateReferenceGraphPass::config.refs_in_code) {
      walk_methods(scope, exception_ref_builder(scope, class_refs));
      walk_opcodes(
        scope,
        [](const DexMethod*) { return true; },
        instruction_ref_builder(scope, class_refs)
      );
    }
  }
}

void CreateReferenceGraphPass::createAndOutputRefGraph(
    DexStore& store,
    type_to_store_map_t type_to_store) {
  refs_t class_refs;
  auto scope = build_class_scope(store.get_dexen());
  build_refs(scope, class_refs);

  for (auto& ref : class_refs) {
    const auto source = ref.first;
    for (const auto& target : ref.second) {
      std::string target_store_name;
      auto find = type_to_store.find(target);
      if (find != type_to_store.end()) {
        target_store_name = find->second->get_name();
      } else {
        target_store_name = "external";
      }
      TRACE(
        ANALYSIS_REF_GRAPH,
        5,
        "%s:%s->%s:%s\n",
        store.get_name().c_str(),
        source->get_deobfuscated_name().c_str(),
        target_store_name.c_str(),
        target->get_name()->c_str());
    }
  }
}

void CreateReferenceGraphPass::run_pass(
    DexStoresVector& stores,
    ConfigFiles& cfg, /* unused */
    PassManager& mgr /* unused */) {

  type_to_store_map_t type_to_store;
  for (auto& store : stores) {
    auto scope = build_class_scope(store.get_dexen());
    for (const auto* cls : scope) {
      type_to_store[cls->get_type()] = &store;
    }
  }

  for (auto& store : stores) {
    createAndOutputRefGraph(store, type_to_store);
  }
}

static CreateReferenceGraphPass s_pass;
