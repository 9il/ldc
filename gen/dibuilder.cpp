//===-- gen/dibuilder.h - Debug information builder -------------*- C++ -*-===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#include "gen/dibuilder.h"

#include "gen/functions.h"
#include "gen/irstate.h"
#include "gen/llvmhelpers.h"
#include "gen/logger.h"
#include "gen/tollvm.h"
#include "gen/optimizer.h"
#include "ir/irfunction.h"
#include "ir/irtypeaggr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "enum.h"
#include "ldcbindings.h"
#include "module.h"
#include "mtype.h"

////////////////////////////////////////////////////////////////////////////////

#if LDC_LLVM_VER >= 306
using LLMetadata = llvm::Metadata;
#else
using LLMetadata = llvm::Value;
#endif

#if LDC_LLVM_VER >= 307
using DIFlags = llvm::DINode;
#else
using DIFlags = llvm::DIDescriptor;
#endif

namespace {
#if LDC_LLVM_VER >= 400
const auto DIFlagZero = DIFlags::FlagZero;
#else
const unsigned DIFlagZero = 0;
#endif

ldc::DIType getNullDIType() {
#if LDC_LLVM_VER >= 307
  return nullptr;
#else
  return llvm::DIType();
#endif
}
}

llvm::StringRef uniqueIdent(Type* t) {
#if LDC_LLVM_VER >= 309
  if (t->deco)
    return t->deco;
#endif
  return llvm::StringRef();
}

////////////////////////////////////////////////////////////////////////////////

// get the module the symbol is in, or - for template instances - the current
// module
Module *ldc::DIBuilder::getDefinedModule(Dsymbol *s) {
  // templates are defined in current module
  if (DtoIsTemplateInstance(s)) {
    return IR->dmodule;
  }
  // array operations as well
  if (FuncDeclaration *fd = s->isFuncDeclaration()) {
    if (fd->isArrayOp && (willInline() || !isDruntimeArrayOp(fd))) {
      return IR->dmodule;
    }
  }
  // otherwise use the symbol's module
  return s->getModule();
}

////////////////////////////////////////////////////////////////////////////////

ldc::DIBuilder::DIBuilder(IRState *const IR)
    : IR(IR), DBuilder(IR->module), CUNode(nullptr) {}

llvm::LLVMContext &ldc::DIBuilder::getContext() { return IR->context(); }

ldc::DIScope ldc::DIBuilder::GetCurrentScope() {
  IrFunction *fn = IR->func();
  if (fn->diLexicalBlocks.empty()) {
    assert(static_cast<llvm::MDNode *>(fn->diSubprogram) != 0);
    return fn->diSubprogram;
  }
  return fn->diLexicalBlocks.top();
}

void ldc::DIBuilder::Declare(const Loc &loc, llvm::Value *var,
                             ldc::DILocalVariable divar
#if LDC_LLVM_VER >= 306
                             ,
                             ldc::DIExpression diexpr
#endif
                             ) {
  unsigned charnum = (loc.linnum ? loc.charnum : 0);
  auto debugLoc = llvm::DebugLoc::get(loc.linnum, charnum, GetCurrentScope());
#if LDC_LLVM_VER < 307
  llvm::Instruction *instr = DBuilder.insertDeclare(var, divar,
#if LDC_LLVM_VER >= 306
                                                    diexpr,
#endif
                                                    IR->scopebb());
  instr->setDebugLoc(debugLoc);
#else // if LLVM >= 3.7
  DBuilder.insertDeclare(var, divar, diexpr, debugLoc, IR->scopebb());
#endif
}

ldc::DIFile ldc::DIBuilder::CreateFile(Loc &loc) {
  llvm::SmallString<128> path(loc.filename ? loc.filename : "");
  llvm::sys::fs::make_absolute(path);

  return DBuilder.createFile(llvm::sys::path::filename(path),
                             llvm::sys::path::parent_path(path));
}

ldc::DIFile ldc::DIBuilder::CreateFile() {
  Loc loc(IR->dmodule->srcfile->toChars(), 0, 0);
  return CreateFile(loc);
}

ldc::DIType ldc::DIBuilder::CreateBasicType(Type *type) {
  using namespace llvm::dwarf;

  Type *t = type->toBasetype();
  llvm::Type *T = DtoType(type);

  // find encoding
  unsigned Encoding;
  switch (t->ty) {
  case Tbool:
    Encoding = DW_ATE_boolean;
    break;
  case Tchar:
    if (global.params.targetTriple->isWindowsMSVCEnvironment()) {
      // VS debugger does not support DW_ATE_UTF for char
      Encoding = DW_ATE_unsigned_char;
      break;
    }
  case Twchar:
  case Tdchar:
    Encoding = DW_ATE_UTF;
    break;
  case Tint8:
    if (global.params.targetTriple->isWindowsMSVCEnvironment()) {
      // VS debugger does not support DW_ATE_signed for 8-bit
      Encoding = DW_ATE_signed_char;
      break;
    }
  case Tint16:
  case Tint32:
  case Tint64:
  case Tint128:
    Encoding = DW_ATE_signed;
    break;
  case Tuns8:
    if (global.params.targetTriple->isWindowsMSVCEnvironment()) {
      // VS debugger does not support DW_ATE_unsigned for 8-bit
      Encoding = DW_ATE_unsigned_char;
      break;
    }
  case Tuns16:
  case Tuns32:
  case Tuns64:
  case Tuns128:
    Encoding = DW_ATE_unsigned;
    break;
  case Tfloat32:
  case Tfloat64:
  case Tfloat80:
    Encoding = DW_ATE_float;
    break;
  case Timaginary32:
  case Timaginary64:
  case Timaginary80:
    if (global.params.targetTriple->isWindowsMSVCEnvironment()) {
      // DW_ATE_imaginary_float not supported by the LLVM DWARF->CodeView
      // conversion
      Encoding = DW_ATE_float;
      break;
    }
    Encoding = DW_ATE_imaginary_float;
    break;
  case Tcomplex32:
  case Tcomplex64:
  case Tcomplex80:
    if (global.params.targetTriple->isWindowsMSVCEnvironment()) {
      // DW_ATE_complex_float not supported by the LLVM DWARF->CodeView
      // conversion
      return CreateComplexType(t);
    }
    Encoding = DW_ATE_complex_float;
    break;
  default:
    llvm_unreachable(
        "Unsupported basic type for debug info in DIBuilder::CreateBasicType");
  }

  return DBuilder.createBasicType(type->toChars(),         // name
                                  getTypeAllocSize(T) * 8, // size (bits)
                                  getABITypeAlign(T) * 8,  // align (bits)
                                  Encoding);
}

ldc::DIType ldc::DIBuilder::CreateEnumType(Type *type) {
  assert(type->ty == Tenum);

  llvm::Type *T = DtoType(type);
  TypeEnum *te = static_cast<TypeEnum *>(type);

  llvm::SmallVector<LLMetadata *, 8> subscripts;
  for (auto m : *te->sym->members) {
    EnumMember *em = m->isEnumMember();
    llvm::StringRef Name(em->toChars());
    uint64_t Val = em->value()->toInteger();
    auto Subscript = DBuilder.createEnumerator(Name, Val);
    subscripts.push_back(Subscript);
  }

  llvm::StringRef Name = te->toChars();
  unsigned LineNumber = te->sym->loc.linnum;
  ldc::DIFile File(CreateFile(te->sym->loc));

  return DBuilder.createEnumerationType(
      GetCU(), Name, File, LineNumber,
      getTypeAllocSize(T) * 8,               // size (bits)
      getABITypeAlign(T) * 8,                // align (bits)
      DBuilder.getOrCreateArray(subscripts), // subscripts
      CreateTypeDescription(te->sym->memtype, false));
}

ldc::DIType ldc::DIBuilder::CreatePointerType(Type *type) {
  llvm::Type *T = DtoType(type);
  Type *t = type->toBasetype();
  assert(t->ty == Tpointer);

  // find base type
  Type *nt = t->nextOf();
  // translate void pointers to byte pointers
  if (nt->toBasetype()->ty == Tvoid)
    nt = Type::tuns8;

  return DBuilder.createPointerType(CreateTypeDescription(nt, false),
                                    getTypeAllocSize(T) * 8, // size (bits)
                                    getABITypeAlign(T) * 8,  // align (bits)
                                    type->toChars()          // name
                                    );
}

ldc::DIType ldc::DIBuilder::CreateVectorType(Type *type) {
  LLType *T = DtoType(type);
  Type *t = type->toBasetype();

  assert(t->ty == Tvector &&
         "Only vectors allowed for debug info in DIBuilder::CreateVectorType");
  TypeVector *tv = static_cast<TypeVector *>(t);
  Type *te = tv->elementType();
  int64_t Dim = tv->size(Loc()) / te->size(Loc());
  LLMetadata *subscripts[] = {DBuilder.getOrCreateSubrange(0, Dim)};

  return DBuilder.createVectorType(
      getTypeAllocSize(T) * 8,              // size (bits)
      getABITypeAlign(T) * 8,               // align (bits)
      CreateTypeDescription(te, false),     // element type
      DBuilder.getOrCreateArray(subscripts) // subscripts
      );
}

ldc::DIType ldc::DIBuilder::CreateComplexType(Type *type) {
    llvm::Type *T = DtoType(type);
    Type *t = type->toBasetype();

    Type* elemtype = nullptr;
    switch (t->ty) {
    case Tcomplex32:
        elemtype = Type::tfloat32;
        break;
    case Tcomplex64:
        elemtype = Type::tfloat64;
        break;
    case Tcomplex80:
        elemtype = Type::tfloat80;
        break;
    default:
        llvm_unreachable(
            "Unexpected type for debug info in DIBuilder::CreateComplexType");
    }
    ldc::DIFile file = CreateFile();

    auto imoffset = getTypeAllocSize(DtoType(elemtype));
    LLMetadata *elems[] = {
        CreateMemberType(0, elemtype, file, "re", 0, PROTpublic),
        CreateMemberType(0, elemtype, file, "im", imoffset, PROTpublic)};

    return DBuilder.createStructType(GetCU(),
                                     t->toChars(),            // Name
                                     file,                    // File
                                     0,                       // LineNo
                                     getTypeAllocSize(T) * 8, // size in bits
                                     getABITypeAlign(T) * 8,  // alignment
                                     DIFlagZero,              // What here?
                                     getNullDIType(),         // derived from
                                     DBuilder.getOrCreateArray(elems),
                                     0,               // RunTimeLang
                                     getNullDIType(), // VTableHolder
                                     uniqueIdent(t)); // UniqueIdentifier
}

ldc::DIType ldc::DIBuilder::CreateMemberType(unsigned linnum, Type *type,
                                             ldc::DIFile file,
                                             const char *c_name,
                                             unsigned offset, PROTKIND prot) {
  Type *t = type->toBasetype();

  // translate functions to function pointers
  if (t->ty == Tfunction)
    t = t->pointerTo();

  llvm::Type *T = DtoType(t);

  // find base type
  ldc::DIType basetype = CreateTypeDescription(t, true);

  auto Flags = DIFlagZero;
  switch (prot) {
  case PROTprivate:
    Flags = DIFlags::FlagPrivate;
    break;
  case PROTprotected:
    Flags = DIFlags::FlagProtected;
    break;
#if LDC_LLVM_VER >= 306
  case PROTpublic:
    Flags = DIFlags::FlagPublic;
    break;
#endif
  default:
    break;
  }

  return DBuilder.createMemberType(GetCU(),
                                   c_name,                  // name
                                   file,                    // file
                                   linnum,                  // line number
                                   getTypeAllocSize(T) * 8, // size (bits)
                                   getABITypeAlign(T) * 8,  // align (bits)
                                   offset * 8,              // offset (bits)
                                   Flags,                   // flags
                                   basetype                 // derived from
                                   );
}

void ldc::DIBuilder::AddBaseFields(ClassDeclaration *sd, ldc::DIFile file,
                                   llvm::SmallVector<LLMetadata *, 16> &elems) {
  if (sd->baseClass)
    AddBaseFields(sd->baseClass, file, elems);

  size_t narr = sd->fields.dim;
  elems.reserve(narr);
  for (auto vd : sd->fields) {
    elems.push_back(CreateMemberType(vd->loc.linnum, vd->type, file,
                                     vd->toChars(), vd->offset,
                                     vd->prot().kind));
  }
}

ldc::DIType ldc::DIBuilder::CreateCompositeType(Type *type) {
  Type *t = type->toBasetype();
  assert((t->ty == Tstruct || t->ty == Tclass) &&
         "Unsupported type for debug info in DIBuilder::CreateCompositeType");
  AggregateDeclaration *sd;
  if (t->ty == Tstruct) {
    TypeStruct *ts = static_cast<TypeStruct *>(t);
    sd = ts->sym;
  } else {
    TypeClass *tc = static_cast<TypeClass *>(t);
    sd = tc->sym;
  }
  assert(sd);

  // Use the actual type associated with the declaration, ignoring any
  // const/wrappers.
  LLType *T = DtoType(sd->type);
  IrTypeAggr *ir = sd->type->ctype->isAggr();
  assert(ir);

  if (static_cast<llvm::MDNode *>(ir->diCompositeType) != nullptr) {
    return ir->diCompositeType;
  }

  // if we don't know the aggregate's size, we don't know enough about it
  // to provide debug info. probably a forward-declared struct?
  if (sd->sizeok == SIZEOKnone) {
    return DBuilder.createUnspecifiedType(sd->toChars());
  }

  // elements
  llvm::SmallVector<LLMetadata *, 16> elems;

  // defaults
  llvm::StringRef name = sd->toChars();
  unsigned linnum = sd->loc.linnum;
  ldc::DICompileUnit CU(GetCU());
  assert(CU && "Compilation unit missing or corrupted");
  ldc::DIFile file = CreateFile(sd->loc);
  ldc::DIType derivedFrom = getNullDIType();

  // set diCompositeType to handle recursive types properly
  unsigned tag = (t->ty == Tstruct) ? llvm::dwarf::DW_TAG_structure_type
                                    : llvm::dwarf::DW_TAG_class_type;
#if LDC_LLVM_VER >= 307
  ir->diCompositeType = DBuilder.createReplaceableCompositeType(
#else
  ir->diCompositeType = DBuilder.createReplaceableForwardDecl(
#endif
      tag, name, CU, file, linnum);

  if (!sd->isInterfaceDeclaration()) // plain interfaces don't have one
  {
    if (t->ty == Tstruct) {
      elems.reserve(sd->fields.dim);
      for (auto vd : sd->fields) {
        ldc::DIType dt =
            CreateMemberType(vd->loc.linnum, vd->type, file, vd->toChars(),
                             vd->offset, vd->prot().kind);
        elems.push_back(dt);
      }
    } else {
      ClassDeclaration *classDecl = sd->isClassDeclaration();
      AddBaseFields(classDecl, file, elems);
      if (classDecl->baseClass)
        derivedFrom = CreateCompositeType(classDecl->baseClass->getType());
    }
  }

  auto elemsArray = DBuilder.getOrCreateArray(elems);

  ldc::DIType ret;
  if (t->ty == Tclass) {
    ret = DBuilder.createClassType(CU,     // compile unit where defined
                                   name,   // name
                                   file,   // file where defined
                                   linnum, // line number where defined
                                   getTypeAllocSize(T) * 8, // size in bits
                                   getABITypeAlign(T) * 8,  // alignment in bits
                                   0,                       // offset in bits,
                                   DIFlagZero,              // flags
                                   derivedFrom,             // DerivedFrom
                                   elemsArray,
                                   getNullDIType(), // VTableHolder
                                   nullptr,         // TemplateParms
                                   uniqueIdent(t)); // UniqueIdentifier
  } else {
    ret = DBuilder.createStructType(CU,     // compile unit where defined
                                    name,   // name
                                    file,   // file where defined
                                    linnum, // line number where defined
                                    getTypeAllocSize(T) * 8, // size in bits
                                    getABITypeAlign(T) * 8, // alignment in bits
                                    DIFlagZero,             // flags
                                    derivedFrom,            // DerivedFrom
                                    elemsArray,
                                    0,               // RunTimeLang
                                    getNullDIType(), // VTableHolder
                                    uniqueIdent(t)); // UniqueIdentifier
  }

#if LDC_LLVM_VER >= 307
  ir->diCompositeType = DBuilder.replaceTemporary(
      llvm::TempDINode(ir->diCompositeType), static_cast<llvm::DIType *>(ret));
#else
  ir->diCompositeType.replaceAllUsesWith(ret);
#endif
  ir->diCompositeType = ret;

  return ret;
}

ldc::DIType ldc::DIBuilder::CreateArrayType(Type *type) {
  llvm::Type *T = DtoType(type);
  Type *t = type->toBasetype();
  assert(t->ty == Tarray);

  ldc::DIFile file = CreateFile();

  LLMetadata *elems[] = {
      CreateMemberType(0, Type::tsize_t, file, "length", 0, PROTpublic),
      CreateMemberType(0, t->nextOf()->pointerTo(), file, "ptr",
                       global.params.is64bit ? 8 : 4, PROTpublic)};

  return DBuilder.createStructType(GetCU(),
                                   t->toChars(),            // Name
                                   file,                    // File
                                   0,                       // LineNo
                                   getTypeAllocSize(T) * 8, // size in bits
                                   getABITypeAlign(T) * 8,  // alignment in bits
                                   DIFlagZero,              // What here?
                                   getNullDIType(),         // derived from
                                   DBuilder.getOrCreateArray(elems),
                                   0,               // RunTimeLang
                                   getNullDIType(), // VTableHolder
                                   uniqueIdent(t)); // UniqueIdentifier
}

ldc::DIType ldc::DIBuilder::CreateSArrayType(Type *type) {
  llvm::Type *T = DtoType(type);
  Type *t = type->toBasetype();
  assert(t->ty == Tsarray);

  // find base type
  llvm::SmallVector<LLMetadata *, 8> subscripts;
  while (t->ty == Tsarray) {
    TypeSArray *tsa = static_cast<TypeSArray *>(t);
    int64_t Count = tsa->dim->toInteger();
    auto subscript = DBuilder.getOrCreateSubrange(0, Count);
    subscripts.push_back(subscript);
    t = t->nextOf();
  }

  // element type: void => byte, function => function pointer
  t = t->toBasetype();
  if (t->ty == Tvoid)
    t = Type::tuns8;
  else if (t->ty == Tfunction)
    t = t->pointerTo();

  return DBuilder.createArrayType(
      getTypeAllocSize(T) * 8,              // size (bits)
      getABITypeAlign(T) * 8,               // align (bits)
      CreateTypeDescription(t, false),      // element type
      DBuilder.getOrCreateArray(subscripts) // subscripts
      );
}

ldc::DIType ldc::DIBuilder::CreateAArrayType(Type *type) {
  return CreatePointerType(Type::tvoidptr);
}

////////////////////////////////////////////////////////////////////////////////

ldc::DISubroutineType ldc::DIBuilder::CreateFunctionType(Type *type) {
  assert(type->toBasetype()->ty == Tfunction);

  TypeFunction *t = static_cast<TypeFunction *>(type);
  Type *retType = t->next;

  // Create "dummy" subroutine type for the return type
  LLMetadata *params = {CreateTypeDescription(retType, true)};
#if LDC_LLVM_VER == 305
  auto paramsArray = DBuilder.getOrCreateArray(params);
#else
  auto paramsArray = DBuilder.getOrCreateTypeArray(params);
#endif

#if LDC_LLVM_VER >= 308
  return DBuilder.createSubroutineType(paramsArray);
#else
  return DBuilder.createSubroutineType(CreateFile(), paramsArray);
#endif
}

ldc::DIType ldc::DIBuilder::CreateDelegateType(Type *type) {
  assert(type->toBasetype()->ty == Tdelegate);

  llvm::Type *T = DtoType(type);
  auto t = static_cast<TypeDelegate *>(type);

  ldc::DICompileUnit CU(GetCU());
  assert(CU && "Compilation unit missing or corrupted");
  auto file = CreateFile();

  LLMetadata *elems[] = {
      CreateMemberType(0, Type::tvoidptr, file, "context", 0, PROTpublic),
      CreateMemberType(0, t->next, file, "funcptr",
                       global.params.is64bit ? 8 : 4, PROTpublic)};

  return DBuilder.createStructType(CU,           // compile unit where defined
                                   t->toChars(), // name
                                   file,         // file where defined
                                   0,            // line number where defined
                                   getTypeAllocSize(T) * 8, // size in bits
                                   getABITypeAlign(T) * 8,  // alignment in bits
                                   DIFlagZero,              // flags
                                   getNullDIType(),         // derived from
                                   DBuilder.getOrCreateArray(elems),
                                   0,               // RunTimeLang
                                   getNullDIType(), // VTableHolder
                                   uniqueIdent(t)); // UniqueIdentifier
}

////////////////////////////////////////////////////////////////////////////////
bool isOpaqueEnumType(Type *type) {
  if (type->ty != Tenum)
    return false;

  TypeEnum *te = static_cast<TypeEnum *>(type);
  return !te->sym->memtype;
}

ldc::DIType ldc::DIBuilder::CreateTypeDescription(Type *type, bool derefclass) {
  // Check for opaque enum first, Bugzilla 13792
  if (isOpaqueEnumType(type))
    return DBuilder.createUnspecifiedType(type->toChars());

  Type *t = type->toBasetype();
  if (derefclass && t->ty == Tclass) {
    type = type->pointerTo();
    t = type->toBasetype();
  }

#if LDC_LLVM_VER >= 309
  if (t->ty == Tnull)
      return DBuilder.createNullPtrType();
  if (t->ty == Tvoid)
      return nullptr;
#else
  if (t->ty == Tvoid || t->ty == Tnull)
    return DBuilder.createUnspecifiedType(t->toChars());
#endif
  if (t->isintegral() || t->isfloating()) {
    if (t->ty == Tvector)
      return CreateVectorType(type);
    if (type->ty == Tenum)
      return CreateEnumType(type);
    return CreateBasicType(type);
  }
  if (t->ty == Tpointer)
    return CreatePointerType(type);
  if (t->ty == Tarray)
    return CreateArrayType(type);
  if (t->ty == Tsarray)
    return CreateSArrayType(type);
  if (t->ty == Taarray)
    return CreateAArrayType(type);
  if (t->ty == Tstruct || t->ty == Tclass)
    return CreateCompositeType(type);
  if (t->ty == Tfunction)
    return CreateFunctionType(type);
  if (t->ty == Tdelegate)
    return CreateDelegateType(type);

  // Crash if the type is not supported.
  llvm_unreachable("Unsupported type in debug info");
}

////////////////////////////////////////////////////////////////////////////////

void ldc::DIBuilder::EmitCompileUnit(Module *m) {
  if (!global.params.symdebug) {
    return;
  }

  Logger::println("D to dwarf compile_unit");
  LOG_SCOPE;

  assert(!CUNode && "Already created compile unit for this DIBuilder instance");

  // prepare srcpath
  llvm::SmallString<128> srcpath(m->srcfile->name->toChars());
  llvm::sys::fs::make_absolute(srcpath);

#if LDC_LLVM_VER >= 308
  if (global.params.targetTriple->isWindowsMSVCEnvironment())
    IR->module.addModuleFlag(llvm::Module::Warning, "CodeView", 1);
#endif
  // Metadata without a correct version will be stripped by UpgradeDebugInfo.
  IR->module.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                           llvm::DEBUG_METADATA_VERSION);

  CUNode = DBuilder.createCompileUnit(
      global.params.symdebug == 2 ? llvm::dwarf::DW_LANG_C
                                  : llvm::dwarf::DW_LANG_D,
      llvm::sys::path::filename(srcpath), llvm::sys::path::parent_path(srcpath),
      "LDC (http://wiki.dlang.org/LDC)",
      isOptimizationEnabled(), // isOptimized
      llvm::StringRef(),       // Flags TODO
      1                        // Runtime Version TODO
      );
}

ldc::DISubprogram ldc::DIBuilder::EmitSubProgram(FuncDeclaration *fd) {
  if (!global.params.symdebug) {
#if LDC_LLVM_VER >= 307
    return nullptr;
#else
    return llvm::DISubprogram();
#endif
  }

  Logger::println("D to dwarf subprogram");
  LOG_SCOPE;

  ldc::DICompileUnit CU(GetCU());
  assert(CU &&
         "Compilation unit missing or corrupted in DIBuilder::EmitSubProgram");

  ldc::DIFile file = CreateFile(fd->loc);

  // Create subroutine type
  ldc::DISubroutineType DIFnType =
      CreateFunctionType(static_cast<TypeFunction *>(fd->type));

  // FIXME: duplicates?
  auto SP = DBuilder.createFunction(
      CU,                                 // context
      fd->toPrettyChars(),                // name
      getIrFunc(fd)->func->getName(),     // linkage name
      file,                               // file
      fd->loc.linnum,                     // line no
      DIFnType,                           // type
      fd->protection.kind == PROTprivate, // is local to unit
      true,                               // isdefinition
      fd->loc.linnum,                     // FIXME: scope line
      DIFlags::FlagPrototyped,            // Flags
      isOptimizationEnabled()             // isOptimized
#if LDC_LLVM_VER < 308
      ,
      getIrFunc(fd)->func
#endif
      );
#if LDC_LLVM_VER >= 308
  if (fd->fbody)
    getIrFunc(fd)->func->setSubprogram(SP);
#endif
  return SP;
}

ldc::DISubprogram ldc::DIBuilder::EmitThunk(llvm::Function *Thunk,
                                            FuncDeclaration *fd) {
  if (!global.params.symdebug) {
#if LDC_LLVM_VER >= 307
    return nullptr;
#else
    return llvm::DISubprogram();
#endif
  }

  Logger::println("Thunk to dwarf subprogram");
  LOG_SCOPE;

  ldc::DICompileUnit CU(GetCU());
  assert(CU && "Compilation unit missing or corrupted in DIBuilder::EmitThunk");

  ldc::DIFile file = CreateFile(fd->loc);

  // Create subroutine type (thunk has same type as wrapped function)
  ldc::DISubroutineType DIFnType = CreateFunctionType(fd->type);

  std::string name = fd->toPrettyChars();
  name.append(".__thunk");

  // FIXME: duplicates?
  auto SP = DBuilder.createFunction(
      CU,                                 // context
      name,                               // name
      Thunk->getName(),                   // linkage name
      file,                               // file
      fd->loc.linnum,                     // line no
      DIFnType,                           // type
      fd->protection.kind == PROTprivate, // is local to unit
      true,                               // isdefinition
      fd->loc.linnum,                     // FIXME: scope line
      DIFlags::FlagPrototyped,            // Flags
      isOptimizationEnabled()             // isOptimized
#if LDC_LLVM_VER < 308
      ,
      getIrFunc(fd)->func
#endif
      );
#if LDC_LLVM_VER >= 308
  if (fd->fbody)
    getIrFunc(fd)->func->setSubprogram(SP);
#endif
  return SP;
}

ldc::DISubprogram ldc::DIBuilder::EmitModuleCTor(llvm::Function *Fn,
                                                 llvm::StringRef prettyname) {
  if (!global.params.symdebug) {
#if LDC_LLVM_VER >= 307
    return nullptr;
#else
    return llvm::DISubprogram();
#endif
  }

  Logger::println("D to dwarf subprogram");
  LOG_SCOPE;

  ldc::DICompileUnit CU(GetCU());
  assert(CU &&
         "Compilation unit missing or corrupted in DIBuilder::EmitSubProgram");
  ldc::DIFile file = CreateFile();

  // Create "dummy" subroutine type for the return type
  LLMetadata *params = {CreateTypeDescription(Type::tvoid, true)};
#if LDC_LLVM_VER >= 306
  auto paramsArray = DBuilder.getOrCreateTypeArray(params);
#else
  auto paramsArray = DBuilder.getOrCreateArray(params);
#endif
#if LDC_LLVM_VER >= 308
  ldc::DISubroutineType DIFnType = DBuilder.createSubroutineType(paramsArray);
#else
  ldc::DISubroutineType DIFnType =
      DBuilder.createSubroutineType(file, paramsArray);
#endif

  // FIXME: duplicates?
  auto SP =
      DBuilder.createFunction(CU,            // context
                              prettyname,    // name
                              Fn->getName(), // linkage name
                              file,          // file
                              0,             // line no
                              DIFnType,      // return type. TODO: fill it up
                              true,          // is local to unit
                              true,          // isdefinition
                              0,             // FIXME: scope line
                              DIFlags::FlagPrototyped | DIFlags::FlagArtificial,
                              isOptimizationEnabled() // isOptimized
#if LDC_LLVM_VER < 308
                              ,
                              Fn
#endif
                              );
#if LDC_LLVM_VER >= 308
  Fn->setSubprogram(SP);
#endif
  return SP;
}

void ldc::DIBuilder::EmitFuncStart(FuncDeclaration *fd) {
  if (!global.params.symdebug)
    return;

  Logger::println("D to dwarf funcstart");
  LOG_SCOPE;

  assert(static_cast<llvm::MDNode *>(getIrFunc(fd)->diSubprogram) != 0);
  EmitStopPoint(fd->loc);
}

void ldc::DIBuilder::EmitFuncEnd(FuncDeclaration *fd) {
  if (!global.params.symdebug)
    return;

  Logger::println("D to dwarf funcend");
  LOG_SCOPE;

  assert(static_cast<llvm::MDNode *>(getIrFunc(fd)->diSubprogram) != 0);
  EmitStopPoint(fd->endloc);
}

void ldc::DIBuilder::EmitBlockStart(Loc &loc) {
  if (!global.params.symdebug)
    return;

  Logger::println("D to dwarf block start");
  LOG_SCOPE;

  ldc::DILexicalBlock block =
      DBuilder.createLexicalBlock(GetCurrentScope(),           // scope
                                  CreateFile(loc),             // file
                                  loc.linnum,                  // line
                                  loc.linnum ? loc.charnum : 0 // column
#if LDC_LLVM_VER == 305
                                  ,
                                  0 // DWARF path discriminator value
#endif
                                  );
  IR->func()->diLexicalBlocks.push(block);
  EmitStopPoint(loc);
}

void ldc::DIBuilder::EmitBlockEnd() {
  if (!global.params.symdebug)
    return;

  Logger::println("D to dwarf block end");
  LOG_SCOPE;

  IrFunction *fn = IR->func();
  assert(!fn->diLexicalBlocks.empty());
  fn->diLexicalBlocks.pop();
}

void ldc::DIBuilder::EmitStopPoint(Loc &loc) {
  if (!global.params.symdebug)
    return;

// If we already have a location set and the current loc is invalid
// (line 0), then we can just ignore it (see GitHub issue #998 for why we
// cannot do this in all cases).
#if LDC_LLVM_VER >= 307
  if (!loc.linnum && IR->ir->getCurrentDebugLocation())
    return;
#else
  if (!loc.linnum && !IR->ir->getCurrentDebugLocation().isUnknown())
    return;
#endif

  unsigned charnum = (loc.linnum ? loc.charnum : 0);
  Logger::println("D to dwarf stoppoint at line %u, column %u", loc.linnum,
                  charnum);
  LOG_SCOPE;
  IR->ir->SetCurrentDebugLocation(
      llvm::DebugLoc::get(loc.linnum, charnum, GetCurrentScope()));
  currentLoc = loc;
}

Loc ldc::DIBuilder::GetCurrentLoc() const { return currentLoc; }

void ldc::DIBuilder::EmitValue(llvm::Value *val, VarDeclaration *vd) {
  auto sub = IR->func()->variableMap.find(vd);
  if (sub == IR->func()->variableMap.end())
    return;

  ldc::DILocalVariable debugVariable = sub->second;
  if (!global.params.symdebug || !debugVariable)
    return;

  llvm::Instruction *instr =
      DBuilder.insertDbgValueIntrinsic(val, 0, debugVariable,
#if LDC_LLVM_VER >= 306
                                       DBuilder.createExpression(),
#endif
#if LDC_LLVM_VER >= 307
                                       IR->ir->getCurrentDebugLocation(),
#endif
                                       IR->scopebb());
  instr->setDebugLoc(IR->ir->getCurrentDebugLocation());
}

void ldc::DIBuilder::EmitLocalVariable(llvm::Value *ll, VarDeclaration *vd,
                                       Type *type, bool isThisPtr,
                                       bool fromNested,
#if LDC_LLVM_VER >= 306
                                       llvm::ArrayRef<int64_t> addr
#else
                                       llvm::ArrayRef<llvm::Value *> addr
#endif
                                       ) {
  if (!global.params.symdebug)
    return;

  Logger::println("D to dwarf local variable");
  LOG_SCOPE;

  auto &variableMap = IR->func()->variableMap;
  auto sub = variableMap.find(vd);
  if (sub != variableMap.end())
    return; // ensure that the debug variable is created only once

  // get type description
  ldc::DIType TD = CreateTypeDescription(type ? type : vd->type, true);
  if (static_cast<llvm::MDNode *>(TD) == nullptr)
    return; // unsupported

  if (vd->storage_class & (STCref | STCout)) {
    TD = DBuilder.createReferenceType(llvm::dwarf::DW_TAG_reference_type, TD);
  }

  // get variable description
  assert(!vd->isDataseg() && "static variable");

#if LDC_LLVM_VER < 308
  unsigned tag;
  if (!fromNested && vd->isParameter()) {
    tag = llvm::dwarf::DW_TAG_arg_variable;
  } else {
    tag = llvm::dwarf::DW_TAG_auto_variable;
  }
#endif

  ldc::DILocalVariable debugVariable;
  auto Flags = !isThisPtr
                   ? DIFlagZero
                   : DIFlags::FlagArtificial | DIFlags::FlagObjectPointer;

#if LDC_LLVM_VER < 306
  if (addr.empty()) {
    debugVariable = DBuilder.createLocalVariable(tag,                 // tag
                                                 GetCurrentScope(),   // scope
                                                 vd->toChars(),       // name
                                                 CreateFile(vd->loc), // file
                                                 vd->loc.linnum, // line num
                                                 TD,             // type
                                                 true,           // preserve
                                                 Flags           // flags
                                                 );
  } else {
    debugVariable = DBuilder.createComplexVariable(tag,                 // tag
                                                   GetCurrentScope(),   // scope
                                                   vd->toChars(),       // name
                                                   CreateFile(vd->loc), // file
                                                   vd->loc.linnum, // line num
                                                   TD,             // type
                                                   addr);
  }
#elif LDC_LLVM_VER < 308
  debugVariable = DBuilder.createLocalVariable(tag,                 // tag
                                               GetCurrentScope(),   // scope
                                               vd->toChars(),       // name
                                               CreateFile(vd->loc), // file
                                               vd->loc.linnum,      // line num
                                               TD,                  // type
                                               true,                // preserve
                                               Flags                // flags
                                               );
#else
  if (!fromNested && vd->isParameter()) {
    FuncDeclaration *fd = vd->parent->isFuncDeclaration();
    assert(fd);
    size_t argNo = 0;
    if (fd->vthis != vd) {
      assert(fd->parameters);
      for (argNo = 0; argNo < fd->parameters->dim; argNo++) {
        if ((*fd->parameters)[argNo] == vd)
          break;
      }
      assert(argNo < fd->parameters->dim);
      if (fd->vthis)
        argNo++;
    }

    debugVariable =
        DBuilder.createParameterVariable(GetCurrentScope(), // scope
                                         vd->toChars(),     // name
                                         argNo + 1,
                                         CreateFile(vd->loc), // file
                                         vd->loc.linnum,      // line num
                                         TD,                  // type
                                         true,                // preserve
                                         Flags                // flags
                                         );
  } else {
    debugVariable = DBuilder.createAutoVariable(GetCurrentScope(),   // scope
                                                vd->toChars(),       // name
                                                CreateFile(vd->loc), // file
                                                vd->loc.linnum,      // line num
                                                TD,                  // type
                                                true,                // preserve
                                                Flags                // flags
                                                );
  }
#endif
  variableMap[vd] = debugVariable;

// declare
#if LDC_LLVM_VER >= 306
  Declare(vd->loc, ll, debugVariable, addr.empty()
                                          ? DBuilder.createExpression()
                                          : DBuilder.createExpression(addr));
#else
  Declare(vd->loc, ll, debugVariable);
#endif
}

void ldc::DIBuilder::EmitGlobalVariable(llvm::GlobalVariable *llVar,
                                        VarDeclaration *vd) {
  if (!global.params.symdebug)
    return;

  Logger::println("D to dwarf global_variable");
  LOG_SCOPE;

  assert(vd->isDataseg() ||
         (vd->storage_class & (STCconst | STCimmutable) && vd->_init));

  auto DIVar = DBuilder.createGlobalVariable(
#if LDC_LLVM_VER >= 306
      GetCU(), // context
#endif
      vd->toChars(),                          // name
      mangle(vd),                             // linkage name
      CreateFile(vd->loc),                    // file
      vd->loc.linnum,                         // line num
      CreateTypeDescription(vd->type, false), // type
      vd->protection.kind == PROTprivate,     // is local to unit
#if LDC_LLVM_VER >= 400
      nullptr // relative location of field
#else
      llVar // value
#endif
      );
#if LDC_LLVM_VER >= 400
  llVar->addDebugInfo(DIVar);
#endif
}

void ldc::DIBuilder::Finalize() {
  if (!global.params.symdebug)
    return;

  DBuilder.finalize();
}
