//===--- GenericEnvironment.cpp - GenericEnvironment AST ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the GenericEnvironment class.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ProtocolConformance.h"

using namespace swift;

GenericEnvironment::GenericEnvironment(
    GenericSignature *signature,
    TypeSubstitutionMap interfaceToArchetypeMap)
  : Signature(signature)
{

  assert(!interfaceToArchetypeMap.empty());
  assert(interfaceToArchetypeMap.size() == signature->getGenericParams().size()
         && "incorrect number of parameters");

  // Build a mapping in both directions, making sure to canonicalize the
  // interface type where it is used as a key, so that substitution can
  // find them, and to preserve sugar otherwise, so that
  // mapTypeOutOfContext() produces a human-readable type.
  for (auto entry : interfaceToArchetypeMap) {
    // We're going to pass InterfaceToArchetypeMap to Type::subst(), which
    // expects the keys to be canonical, otherwise it won't be able to
    // find them.
    auto canParamTy = cast<GenericTypeParamType>(entry.first->getCanonicalType());
    auto contextTy = entry.second;

    auto result = InterfaceToArchetypeMap.insert(
        std::make_pair(canParamTy, contextTy));
    assert(result.second && "duplicate generic parameters in environment");

    // If we mapped the generic parameter to an archetype, add it to the
    // reverse mapping.
    if (auto *archetypeTy = entry.second->getAs<ArchetypeType>())
      ArchetypeToInterfaceMap[archetypeTy] = entry.first;

    // FIXME: If multiple generic parameters map to the same archetype,
    // the reverse mapping order is not deterministic.
  }
}

void *GenericEnvironment::operator new(size_t bytes, const ASTContext &ctx) {
  return ctx.Allocate(bytes, alignof(GenericEnvironment), AllocationArena::Permanent);
}

bool GenericEnvironment::containsPrimaryArchetype(
                                              ArchetypeType *archetype) const {
  return ArchetypeToInterfaceMap.count(archetype) > 0;
}

Type GenericEnvironment::mapTypeOutOfContext(ModuleDecl *M, Type type) const {
  type = type.subst(M, ArchetypeToInterfaceMap, SubstFlags::AllowLoweredTypes);
  assert(!type->hasArchetype() && "not fully substituted");
  return type;
}

Type GenericEnvironment::mapTypeIntoContext(ModuleDecl *M, Type type) const {
  type = type.subst(M, InterfaceToArchetypeMap, SubstFlags::AllowLoweredTypes);
  assert((!type->hasTypeParameter() || type->hasError()) &&
         "not fully substituted");
  return type;
}

Type GenericEnvironment::mapTypeIntoContext(GenericTypeParamType *type) const {
  auto canTy = type->getCanonicalType();
  auto found =
    InterfaceToArchetypeMap.find(canTy->castTo<GenericTypeParamType>());
  assert(found != InterfaceToArchetypeMap.end() &&
         "missing generic parameter");
  return found->second;
}

GenericTypeParamType *GenericEnvironment::getSugaredType(
    GenericTypeParamType *type) const {
  for (auto *sugaredType : getGenericParams())
    if (sugaredType->isEqual(type))
      return sugaredType;

  llvm_unreachable("missing generic parameter");
}

ArrayRef<Substitution>
GenericEnvironment::getForwardingSubstitutions(ModuleDecl *M) const {
  auto lookupConformanceFn =
      [&](CanType original, Type replacement, ProtocolType *protoType)
          -> ProtocolConformanceRef {
    return ProtocolConformanceRef(protoType->getDecl());
  };

  SmallVector<Substitution, 4> result;
  getGenericSignature()->getSubstitutions(*M, InterfaceToArchetypeMap,
                                          lookupConformanceFn, result);
  return getGenericSignature()->getASTContext().AllocateCopy(result);
}

SubstitutionMap GenericEnvironment::
getSubstitutionMap(ModuleDecl *mod,
                   ArrayRef<Substitution> subs) const {
  SubstitutionMap result;
  getSubstitutionMap(mod, subs, result);
  return result;
}

void GenericEnvironment::
getSubstitutionMap(ModuleDecl *mod,
                   ArrayRef<Substitution> subs,
                   SubstitutionMap &result) const {
  for (auto depTy : getGenericSignature()->getAllDependentTypes()) {

    // Map the interface type to a context type.
    auto contextTy = depTy.subst(mod, InterfaceToArchetypeMap, SubstOptions());
    auto *archetype = contextTy->castTo<ArchetypeType>();

    auto sub = subs.front();
    subs = subs.slice(1);

    // Record the replacement type and its conformances.
    result.addSubstitution(CanType(archetype), sub.getReplacement());
    result.addConformances(CanType(archetype), sub.getConformances());
  }

  for (auto reqt : getGenericSignature()->getRequirements()) {
    if (reqt.getKind() != RequirementKind::SameType)
      continue;

    auto first = reqt.getFirstType()->getAs<DependentMemberType>();
    auto second = reqt.getSecondType()->getAs<DependentMemberType>();

    if (!first || !second)
      continue;

    auto archetype = mapTypeIntoContext(mod, first)->getAs<ArchetypeType>();
    if (!archetype)
      continue;

    auto firstBase = first->getBase();
    auto secondBase = second->getBase();

    auto firstBaseArchetype = mapTypeIntoContext(mod, firstBase)->getAs<ArchetypeType>();
    auto secondBaseArchetype = mapTypeIntoContext(mod, secondBase)->getAs<ArchetypeType>();

    if (!firstBaseArchetype || !secondBaseArchetype)
      continue;

    if (archetype->getParent() != firstBaseArchetype)
      result.addParent(CanType(archetype),
                       CanType(firstBaseArchetype),
                       first->getAssocType());
    if (archetype->getParent() != secondBaseArchetype)
      result.addParent(CanType(archetype),
                       CanType(secondBaseArchetype),
                       second->getAssocType());
  }

  assert(subs.empty() && "did not use all substitutions?!");
}
