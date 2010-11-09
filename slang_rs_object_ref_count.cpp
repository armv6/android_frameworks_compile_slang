/*
 * Copyright 2010, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "slang_rs_object_ref_count.h"

#include "clang/AST/DeclGroup.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtVisitor.h"

#include "slang_rs.h"
#include "slang_rs_export_type.h"

using namespace slang;

clang::FunctionDecl *RSObjectRefCount::Scope::
    RSSetObjectFD[RSExportPrimitiveType::LastRSObjectType -
                  RSExportPrimitiveType::FirstRSObjectType + 1];
clang::FunctionDecl *RSObjectRefCount::Scope::
    RSClearObjectFD[RSExportPrimitiveType::LastRSObjectType -
                    RSExportPrimitiveType::FirstRSObjectType + 1];

void RSObjectRefCount::Scope::GetRSRefCountingFunctions(
    clang::ASTContext &C) {
  for (unsigned i = 0;
       i < (sizeof(RSClearObjectFD) / sizeof(clang::FunctionDecl*));
       i++) {
    RSSetObjectFD[i] = NULL;
    RSClearObjectFD[i] = NULL;
  }

  clang::TranslationUnitDecl *TUDecl = C.getTranslationUnitDecl();

  for (clang::DeclContext::decl_iterator I = TUDecl->decls_begin(),
          E = TUDecl->decls_end(); I != E; I++) {
    if ((I->getKind() >= clang::Decl::firstFunction) &&
        (I->getKind() <= clang::Decl::lastFunction)) {
      clang::FunctionDecl *FD = static_cast<clang::FunctionDecl*>(*I);

      // points to RSSetObjectFD or RSClearObjectFD
      clang::FunctionDecl **RSObjectFD;

      if (FD->getName() == "rsSetObject") {
        assert((FD->getNumParams() == 2) &&
               "Invalid rsSetObject function prototype (# params)");
        RSObjectFD = RSSetObjectFD;
      } else if (FD->getName() == "rsClearObject") {
        assert((FD->getNumParams() == 1) &&
               "Invalid rsClearObject function prototype (# params)");
        RSObjectFD = RSClearObjectFD;
      }
      else {
        continue;
      }

      const clang::ParmVarDecl *PVD = FD->getParamDecl(0);
      clang::QualType PVT = PVD->getOriginalType();
      // The first parameter must be a pointer like rs_allocation*
      assert(PVT->isPointerType() &&
             "Invalid rs{Set,Clear}Object function prototype (pointer param)");

      // The rs object type passed to the FD
      clang::QualType RST = PVT->getPointeeType();
      RSExportPrimitiveType::DataType DT =
          RSExportPrimitiveType::GetRSSpecificType(RST.getTypePtr());
      assert(RSExportPrimitiveType::IsRSObjectType(DT)
             && "must be RS object type");

      RSObjectFD[(DT - RSExportPrimitiveType::FirstRSObjectType)] = FD;
    }
  }
}

void RSObjectRefCount::Scope::AppendToCompoundStatement(
    clang::ASTContext& C, std::list<clang::Expr*> &ExprList) {
  // Destructor code will be inserted before any return statement.
  // Any subsequent statements in the compound statement are then placed
  // after our new code.
  // TODO: This should also handle the case of goto/break/continue.
  clang::CompoundStmt::body_iterator bI = mCS->body_begin();
  clang::CompoundStmt::body_iterator bE = mCS->body_end();

  unsigned OldStmtCount = 0;
  for ( ; bI != bE; bI++) {
    OldStmtCount++;
  }

  unsigned NewExprCount = ExprList.size();

  clang::Stmt **StmtList;
  StmtList = new clang::Stmt*[OldStmtCount+NewExprCount];

  unsigned UpdatedStmtCount = 0;
  for (bI = mCS->body_begin(); bI != bE; bI++) {
    if ((*bI)->getStmtClass() == clang::Stmt::ReturnStmtClass) {
      break;
    }
    StmtList[UpdatedStmtCount++] = *bI;
  }

  std::list<clang::Expr*>::const_iterator E = ExprList.end();
  for (std::list<clang::Expr*>::const_iterator I = ExprList.begin(),
          E = ExprList.end();
       I != E;
       I++) {
    StmtList[UpdatedStmtCount++] = *I;
  }

  // Pick up anything left over after a return statement
  for ( ; bI != bE; bI++) {
    StmtList[UpdatedStmtCount++] = *bI;
  }

  mCS->setStmts(C, StmtList, UpdatedStmtCount);
  assert(UpdatedStmtCount == (OldStmtCount + NewExprCount));

  delete [] StmtList;

  return;
}

void RSObjectRefCount::Scope::InsertLocalVarDestructors() {
  std::list<clang::Expr*> RSClearObjectCalls;
  for (std::list<clang::VarDecl*>::const_iterator I = mRSO.begin(),
          E = mRSO.end();
        I != E;
        I++) {
    clang::Expr *E = ClearRSObject(*I);
    if (E) {
      RSClearObjectCalls.push_back(E);
    }
  }
  if (RSClearObjectCalls.size() > 0) {
    clang::ASTContext &C = (*mRSO.begin())->getASTContext();
    AppendToCompoundStatement(C, RSClearObjectCalls);
    // TODO: This should also be extended to append destructors to any
    // further nested scope (we need another visitor here from within the
    // current compound statement in case they call return/goto).
  }
  return;
}

clang::Expr *RSObjectRefCount::Scope::ClearRSObject(clang::VarDecl *VD) {
  clang::ASTContext &C = VD->getASTContext();
  clang::SourceLocation Loc = VD->getLocation();
  const clang::Type *T = RSExportType::GetTypeOfDecl(VD);
  RSExportPrimitiveType::DataType DT =
      RSExportPrimitiveType::GetRSSpecificType(T);

  assert((RSExportPrimitiveType::IsRSObjectType(DT)) &&
      "Should be RS object");

  // Find the rsClearObject() for VD of RS object type DT
  clang::FunctionDecl *ClearObjectFD =
      RSClearObjectFD[(DT - RSExportPrimitiveType::FirstRSObjectType)];
  assert((ClearObjectFD != NULL) &&
      "rsClearObject doesn't cover all RS object types");

  clang::QualType ClearObjectFDType = ClearObjectFD->getType();
  clang::QualType ClearObjectFDArgType =
      ClearObjectFD->getParamDecl(0)->getOriginalType();

  // We generate a call to rsClearObject passing &VD as the parameter
  // (CallExpr 'void'
  //   (ImplicitCastExpr 'void (*)(rs_font *)' <FunctionToPointerDecay>
  //     (DeclRefExpr 'void (rs_font *)' FunctionDecl='rsClearObject'))
  //   (UnaryOperator 'rs_font *' prefix '&'
  //     (DeclRefExpr 'rs_font':'rs_font' Var='[var name]')))

  // Reference expr to target RS object variable
  clang::DeclRefExpr *RefRSVar =
      clang::DeclRefExpr::Create(C,
                                 NULL,
                                 VD->getQualifierRange(),
                                 VD,
                                 Loc,
                                 T->getCanonicalTypeInternal(),
                                 NULL);

  // Get address of RSObject in VD
  clang::Expr *AddrRefRSVar =
      new (C) clang::UnaryOperator(RefRSVar,
                                   clang::UO_AddrOf,
                                   ClearObjectFDArgType,
                                   Loc);

  clang::Expr *RefRSClearObjectFD =
      clang::DeclRefExpr::Create(C,
                                 NULL,
                                 ClearObjectFD->getQualifierRange(),
                                 ClearObjectFD,
                                 ClearObjectFD->getLocation(),
                                 ClearObjectFDType,
                                 NULL);

  clang::Expr *RSClearObjectFP =
      clang::ImplicitCastExpr::Create(C,
                                      C.getPointerType(ClearObjectFDType),
                                      clang::CK_FunctionToPointerDecay,
                                      RefRSClearObjectFD,
                                      NULL,
                                      clang::VK_RValue);

  clang::CallExpr *RSClearObjectCall =
      new (C) clang::CallExpr(C,
                              RSClearObjectFP,
                              &AddrRefRSVar,
                              1,
                              ClearObjectFD->getCallResultType(),
                              clang::SourceLocation());

  return RSClearObjectCall;
}

bool RSObjectRefCount::InitializeRSObject(clang::VarDecl *VD) {
  const clang::Type *T = RSExportType::GetTypeOfDecl(VD);
  RSExportPrimitiveType::DataType DT =
      RSExportPrimitiveType::GetRSSpecificType(T);

  if (DT == RSExportPrimitiveType::DataTypeUnknown)
    return false;

  if (VD->hasInit()) {
    // TODO: Update the reference count of RS object in initializer.
    // This can potentially be done as part of the assignment pass.
  } else {
    clang::Expr *ZeroInitializer =
        CreateZeroInitializerForRSSpecificType(DT,
                                               VD->getASTContext(),
                                               VD->getLocation());

    if (ZeroInitializer) {
      ZeroInitializer->setType(T->getCanonicalTypeInternal());
      VD->setInit(ZeroInitializer);
    }
  }

  return RSExportPrimitiveType::IsRSObjectType(DT);
}

clang::Expr *RSObjectRefCount::CreateZeroInitializerForRSSpecificType(
    RSExportPrimitiveType::DataType DT,
    clang::ASTContext &C,
    const clang::SourceLocation &Loc) {
  clang::Expr *Res = NULL;
  switch (DT) {
    case RSExportPrimitiveType::DataTypeRSElement:
    case RSExportPrimitiveType::DataTypeRSType:
    case RSExportPrimitiveType::DataTypeRSAllocation:
    case RSExportPrimitiveType::DataTypeRSSampler:
    case RSExportPrimitiveType::DataTypeRSScript:
    case RSExportPrimitiveType::DataTypeRSMesh:
    case RSExportPrimitiveType::DataTypeRSProgramFragment:
    case RSExportPrimitiveType::DataTypeRSProgramVertex:
    case RSExportPrimitiveType::DataTypeRSProgramRaster:
    case RSExportPrimitiveType::DataTypeRSProgramStore:
    case RSExportPrimitiveType::DataTypeRSFont: {
      //    (ImplicitCastExpr 'nullptr_t'
      //      (IntegerLiteral 0)))
      llvm::APInt Zero(C.getTypeSize(C.IntTy), 0);
      clang::Expr *Int0 = clang::IntegerLiteral::Create(C, Zero, C.IntTy, Loc);
      clang::Expr *CastToNull =
          clang::ImplicitCastExpr::Create(C,
                                          C.NullPtrTy,
                                          clang::CK_IntegralToPointer,
                                          Int0,
                                          NULL,
                                          clang::VK_RValue);

      Res = new (C) clang::InitListExpr(C, Loc, &CastToNull, 1, Loc);
      break;
    }
    case RSExportPrimitiveType::DataTypeRSMatrix2x2:
    case RSExportPrimitiveType::DataTypeRSMatrix3x3:
    case RSExportPrimitiveType::DataTypeRSMatrix4x4: {
      // RS matrix is not completely an RS object. They hold data by themselves.
      // (InitListExpr rs_matrix2x2
      //   (InitListExpr float[4]
      //     (FloatingLiteral 0)
      //     (FloatingLiteral 0)
      //     (FloatingLiteral 0)
      //     (FloatingLiteral 0)))
      clang::QualType FloatTy = C.FloatTy;
      // Constructor sets value to 0.0f by default
      llvm::APFloat Val(C.getFloatTypeSemantics(FloatTy));
      clang::FloatingLiteral *Float0Val =
          clang::FloatingLiteral::Create(C,
                                         Val,
                                         /* isExact = */true,
                                         FloatTy,
                                         Loc);

      unsigned N = 0;
      if (DT == RSExportPrimitiveType::DataTypeRSMatrix2x2)
        N = 2;
      else if (DT == RSExportPrimitiveType::DataTypeRSMatrix3x3)
        N = 3;
      else if (DT == RSExportPrimitiveType::DataTypeRSMatrix4x4)
        N = 4;

      // Directly allocate 16 elements instead of dynamically allocate N*N
      clang::Expr *InitVals[16];
      for (unsigned i = 0; i < sizeof(InitVals) / sizeof(InitVals[0]); i++)
        InitVals[i] = Float0Val;
      clang::Expr *InitExpr =
          new (C) clang::InitListExpr(C, Loc, InitVals, N * N, Loc);
      InitExpr->setType(C.getConstantArrayType(FloatTy,
                                               llvm::APInt(32, 4),
                                               clang::ArrayType::Normal,
                                               /* EltTypeQuals = */0));

      Res = new (C) clang::InitListExpr(C, Loc, &InitExpr, 1, Loc);
      break;
    }
    case RSExportPrimitiveType::DataTypeUnknown:
    case RSExportPrimitiveType::DataTypeFloat16:
    case RSExportPrimitiveType::DataTypeFloat32:
    case RSExportPrimitiveType::DataTypeFloat64:
    case RSExportPrimitiveType::DataTypeSigned8:
    case RSExportPrimitiveType::DataTypeSigned16:
    case RSExportPrimitiveType::DataTypeSigned32:
    case RSExportPrimitiveType::DataTypeSigned64:
    case RSExportPrimitiveType::DataTypeUnsigned8:
    case RSExportPrimitiveType::DataTypeUnsigned16:
    case RSExportPrimitiveType::DataTypeUnsigned32:
    case RSExportPrimitiveType::DataTypeUnsigned64:
    case RSExportPrimitiveType::DataTypeBoolean:
    case RSExportPrimitiveType::DataTypeUnsigned565:
    case RSExportPrimitiveType::DataTypeUnsigned5551:
    case RSExportPrimitiveType::DataTypeUnsigned4444:
    case RSExportPrimitiveType::DataTypeMax: {
      assert(false && "Not RS object type!");
    }
    // No default case will enable compiler detecting the missing cases
  }

  return Res;
}

void RSObjectRefCount::VisitDeclStmt(clang::DeclStmt *DS) {
  for (clang::DeclStmt::decl_iterator I = DS->decl_begin(), E = DS->decl_end();
       I != E;
       I++) {
    clang::Decl *D = *I;
    if (D->getKind() == clang::Decl::Var) {
      clang::VarDecl *VD = static_cast<clang::VarDecl*>(D);
      if (InitializeRSObject(VD))
        getCurrentScope()->addRSObject(VD);
    }
  }
  return;
}

void RSObjectRefCount::VisitCompoundStmt(clang::CompoundStmt *CS) {
  if (!CS->body_empty()) {
    // Push a new scope
    Scope *S = new Scope(CS);
    mScopeStack.push(S);

    VisitStmt(CS);

    // Destroy the scope
    // TODO: Update reference count of the RS object refenced by
    //       getCurrentScope().
    assert((getCurrentScope() == S) && "Corrupted scope stack!");
    S->InsertLocalVarDestructors();
    mScopeStack.pop();
    delete S;
  }
  return;
}

void RSObjectRefCount::VisitBinAssign(clang::BinaryOperator *AS) {
  // TODO: Update reference count
  return;
}

void RSObjectRefCount::VisitStmt(clang::Stmt *S) {
  for (clang::Stmt::child_iterator I = S->child_begin(), E = S->child_end();
       I != E;
       I++) {
    if (clang::Stmt *Child = *I) {
      Visit(Child);
    }
  }
  return;
}


