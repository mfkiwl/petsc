#include <petsc/private/petschpddm.h>

static PetscBool citeKSP = PETSC_FALSE;
static const char hpddmCitationKSP[] = "@inproceedings{jolivet2016block,\n\tTitle = {{Block Iterative Methods and Recycling for Improved Scalability of Linear Solvers}},\n\tAuthor = {Jolivet, Pierre and Tournier, Pierre-Henri},\n\tOrganization = {IEEE},\n\tYear = {2016},\n\tSeries = {SC16},\n\tBooktitle = {Proceedings of the 2016 International Conference for High Performance Computing, Networking, Storage and Analysis}\n}\n";

static PetscErrorCode KSPSetFromOptions_HPDDM(PetscOptionItems *PetscOptionsObject, KSP ksp)
{
  PetscReal      r;
  PetscInt       i;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  ierr = PetscOptionsHead(PetscOptionsObject, "KSPHPDDM options, cf. https://github.com/hpddm/hpddm");CHKERRQ(ierr);
  ierr = PetscOptionsReal("-ksp_richardson_scale", "Damping factor used in Richardson iterations", "none", 1.0, &r, NULL);CHKERRQ(ierr);
  ierr = PetscOptionsInt("-ksp_gmres_restart", "Maximum number of Arnoldi vectors generated per cycle", "none", 40, &i, NULL);CHKERRQ(ierr);
  ierr = PetscOptionsEList("-ksp_hpddm_krylov_method", "Type of Krylov method", "none", HPDDMKrylovMethod, 7, HPDDMKrylovMethod[HPDDM_KRYLOV_METHOD_GMRES], &i, NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal("-ksp_hpddm_deflation_tol", "Tolerance when deflating right-hand sides inside block methods", "none", -1.0, &r, NULL);CHKERRQ(ierr);
  ierr = PetscOptionsInt("-ksp_hpddm_enlarge_krylov_subspace", "Split the initial right-hand side into multiple vectors", "none", 1, &i, NULL);CHKERRQ(ierr);
  ierr = PetscOptionsEList("-ksp_hpddm_orthogonalization", "Classical (faster) or Modified (more robust) Gram--Schmidt process", "none", HPDDMOrthogonalization, 2, HPDDMOrthogonalization[HPDDM_ORTHOGONALIZATION_CGS], &i, NULL);CHKERRQ(ierr);
  ierr = PetscOptionsEList("-ksp_hpddm_qr", "Distributed QR factorizations computed with Cholesky QR, Classical or Modified Gram--Schmidt process", "none", HPDDMQR, 3, HPDDMQR[HPDDM_QR_CHOLQR], &i, NULL);CHKERRQ(ierr);
  i = HPDDM_VARIANT_LEFT;
  ierr = PetscOptionsEList("-ksp_hpddm_variant", "Left, right, or variable preconditioning", "none", HPDDMVariant, 3, HPDDMVariant[HPDDM_VARIANT_LEFT], &i, NULL);CHKERRQ(ierr);
  if (i > 0) {
    ierr = KSPSetPCSide(ksp, PC_RIGHT);CHKERRQ(ierr);
  }
  ierr = PetscOptionsInt("-ksp_hpddm_recycle", "Number of harmonic Ritz vectors to compute", "none", 0, &i, NULL);CHKERRQ(ierr);
  ierr = PetscOptionsEList("-ksp_hpddm_recycle_target", "Criterion to select harmonic Ritz vectors", "none", HPDDMRecycleTarget, 6, HPDDMRecycleTarget[HPDDM_RECYCLE_TARGET_SM], &i, NULL);CHKERRQ(ierr);
  ierr = PetscOptionsEList("-ksp_hpddm_recycle_strategy", "Generalized eigenvalue problem to solve for recycling", "none", HPDDMRecycleStrategy, 2, HPDDMRecycleStrategy[HPDDM_RECYCLE_STRATEGY_A], &i, NULL);CHKERRQ(ierr);
  ierr = PetscOptionsTail();CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

static PetscErrorCode KSPSetUp_HPDDM(KSP ksp)
{
  Mat            A;
  PetscInt       n;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  ierr = KSPGetOperators(ksp, &A, NULL);CHKERRQ(ierr);
  ierr = MatGetLocalSize(A, &n, NULL);CHKERRQ(ierr);
  HPDDM::PETScOperator *op = new HPDDM::PETScOperator(ksp, n, 1);
  ksp->data = reinterpret_cast<void*>(op);
  PetscFunctionReturn(0);
}

static PetscErrorCode KSPReset_HPDDM(KSP ksp)
{
  PetscFunctionBegin;
  if (ksp->data) {
    delete reinterpret_cast<HPDDM::PETScOperator*>(ksp->data);
    ksp->data = NULL;
  }
  PetscFunctionReturn(0);
}

static PetscErrorCode KSPDestroy_HPDDM(KSP ksp)
{
  PetscErrorCode ierr;

  PetscFunctionBegin;
  ierr = KSPReset_HPDDM(ksp);CHKERRQ(ierr);
  ierr = KSPDestroyDefault(ksp);CHKERRQ(ierr);
  ierr = PetscObjectComposeFunction((PetscObject)ksp, "KSPHPDDMSetDeflationSpace_C", NULL);CHKERRQ(ierr);
  ierr = PetscObjectComposeFunction((PetscObject)ksp, "KSPHPDDMGetDeflationSpace_C", NULL);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

static PetscErrorCode KSPSolve_HPDDM(KSP ksp)
{
  PetscScalar       *x;
  const PetscScalar *b;
  MPI_Comm          comm;
  PetscErrorCode    ierr;

  PetscFunctionBegin;
  ierr = PetscCitationsRegister(hpddmCitationKSP, &citeKSP);CHKERRQ(ierr);
  ierr = VecGetArray(ksp->vec_sol, &x);CHKERRQ(ierr);
  ierr = VecGetArrayRead(ksp->vec_rhs, &b);CHKERRQ(ierr);
  ierr = PetscObjectGetComm((PetscObject)ksp, &comm);CHKERRQ(ierr);
  const HPDDM::PETScOperator& op = *reinterpret_cast<HPDDM::PETScOperator*>(ksp->data);
  ksp->its = HPDDM::IterativeMethod::solve(op, b, x, 1, comm);
  ierr = VecRestoreArrayRead(ksp->vec_rhs, &b);CHKERRQ(ierr);
  ierr = VecRestoreArray(ksp->vec_sol, &x);CHKERRQ(ierr);
  if (ksp->its < ksp->max_it) ksp->reason = KSP_CONVERGED_RTOL;
  else ksp->reason = KSP_DIVERGED_ITS;
  PetscFunctionReturn(0);
}

/*@C
     KSPHPDDMSetDeflationSpace - Sets the deflation space used by Krylov methods with recycling. This space is viewed as a set of vectors stored in a MATDENSE (column major).

   Input Parameters:
+     ksp - iterative context
-     U - deflation space to be used during KSPSolve()

   Level: intermediate

.seealso:  KSPCreate(), KSPType (for list of available types), KSPHPDDMGetDeflationSpace()
@*/
PetscErrorCode KSPHPDDMSetDeflationSpace(KSP ksp, Mat U)
{
  PetscErrorCode ierr;

  PetscFunctionBegin;
  PetscValidHeaderSpecific(ksp, KSP_CLASSID, 1);
  PetscValidHeaderSpecific(U, MAT_CLASSID, 2);
  PetscCheckSameComm(ksp, 1, U, 2);
  ierr = PetscUseMethod(ksp, "KSPHPDDMSetDeflationSpace_C", (KSP, Mat), (ksp, U));CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

/*@C
     KSPHPDDMGetDeflationSpace - Gets the deflation space computed by Krylov methods with recycling or NULL if KSPSolve() has not been called yet. This space is viewed as a set of vectors stored in a MATDENSE (column major). It is the responsibility of the user to free the returned Mat.

   Input Parameter:
+     ksp - iterative context

   Output Parameter:
+     U - deflation space generated during KSPSolve()

   Level: intermediate

.seealso:  KSPCreate(), KSPType (for list of available types), KSPHPDDMSetDeflationSpace()
@*/
PetscErrorCode KSPHPDDMGetDeflationSpace(KSP ksp, Mat *U)
{
  PetscErrorCode ierr;

  PetscFunctionBegin;
  PetscValidHeaderSpecific(ksp, KSP_CLASSID, 1);
  ierr = PetscUseMethod(ksp, "KSPHPDDMGetDeflationSpace_C", (KSP, Mat*), (ksp, U));CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

static PetscErrorCode KSPHPDDMSetDeflationSpace_HPDDM(KSP ksp, Mat U)
{
  HPDDM::PETScOperator *op = reinterpret_cast<HPDDM::PETScOperator*>(ksp->data);
  Mat                  A, local;
  const PetscScalar    *array;
  PetscScalar          *copy;
  PetscInt             m1, M1, m2, M2, n2, N2, ldu;
  PetscBool            match;
  PetscErrorCode       ierr;

  PetscFunctionBegin;
  ierr = KSPGetOperators(ksp, &A, NULL);CHKERRQ(ierr);
  ierr = MatGetLocalSize(A, &m1, NULL);CHKERRQ(ierr);
  ierr = MatGetLocalSize(U, &m2, &n2);CHKERRQ(ierr);
  ierr = MatGetSize(A, &M1, NULL);CHKERRQ(ierr);
  ierr = MatGetSize(U, &M2, &N2);CHKERRQ(ierr);
  if (m1 != m2 || M1 != M2) SETERRQ4(PETSC_COMM_SELF, PETSC_ERR_ARG_SIZ, "Cannot use a deflation space with (m2,M2) = (%D,%D) for a linear system with (m1,M1) = (%D,%D)", m2, M2, m1, M1);
  ierr = PetscObjectTypeCompareAny((PetscObject)U, &match, MATSEQDENSE, MATMPIDENSE, "");CHKERRQ(ierr);
  if (!match) SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_WRONG, "Provided deflation space not stored in a dense Mat");
  ierr = MatDenseGetLocalMatrix(U, &local);CHKERRQ(ierr);
  ierr = MatDenseGetArrayRead(local, &array);CHKERRQ(ierr);
  copy = op->allocate(m2, 1, N2);
  if (!copy) SETERRQ(PETSC_COMM_SELF, PETSC_ERR_POINTER, "Memory allocation error");
  ierr = MatDenseGetLDA(local, &ldu);CHKERRQ(ierr);
  HPDDM::Wrapper<PetscScalar>::omatcopy<'N'>(N2, m2, array, ldu, copy, m2);
  ierr = MatDenseRestoreArrayRead(local, &array);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

static PetscErrorCode KSPHPDDMGetDeflationSpace_HPDDM(KSP ksp, Mat *U)
{
  HPDDM::PETScOperator *op = reinterpret_cast<HPDDM::PETScOperator*>(ksp->data);
  Mat                  A;
  const PetscScalar    *array = op->storage();
  PetscScalar          *copy;
  PetscInt             m1, M1, N2 = op->k();
  PetscErrorCode       ierr;

  PetscFunctionBegin;
  if (!array) *U = NULL;
  else {
    ierr = KSPGetOperators(ksp, &A, NULL);CHKERRQ(ierr);
    ierr = MatGetLocalSize(A, &m1, NULL);CHKERRQ(ierr);
    ierr = MatGetSize(A, &M1, NULL);CHKERRQ(ierr);
    ierr = MatCreateDense(PetscObjectComm((PetscObject)ksp), m1, PETSC_DECIDE, M1, N2, NULL, U);CHKERRQ(ierr);
    ierr = MatDenseGetArray(*U, &copy);CHKERRQ(ierr);
    ierr = PetscArraycpy(copy, array, m1 * N2);CHKERRQ(ierr);
    ierr = MatDenseRestoreArray(*U, &copy);CHKERRQ(ierr);
  }
  PetscFunctionReturn(0);
}

/*MC
     KSPHPDDM - Interface with the HPDDM library.

   This KSP may be used to further select methods that are currently not implemented natively in PETSc, e.g., GCRODR [2006], a recycled Krylov method which is similar to KSPLGMRES, see [2016] for a comparison. ex75.c shows how to reproduce the results from the aforementioned paper [2006]. A chronological bibliography of relevant publications linked with KSP available in HPDDM through KSPHPDDM, and not available directly in PETSc, may be found below.

   Options Database Keys:
+   -ksp_richardson_scale <scale, default=1.0> - see KSPRICHARDSON
.   -ksp_gmres_restart <restart, default=40> - see KSPGMRES
.   -ksp_hpddm_krylov_method <type, default=gmres> - any of gmres, bgmres, cg, bcg, gcrodr, bgcrodr, or bfbcg
.   -ksp_hpddm_deflation_tol <eps, default=\-1.0> - tolerance when deflating right-hand sides inside block methods (no deflation by default, only relevant with block methods)
.   -ksp_hpddm_enlarge_krylov_subspace <p, default=1> - split the initial right-hand side into multiple vectors (only relevant with nonblock methods)
.   -ksp_hpddm_orthogonalization <type, default=cgs> - any of cgs or mgs, see KSPGMRES
.   -ksp_hpddm_qr <type, default=cholqr> - distributed QR factorizations with any of cholqr, cgs, or mgs (only relevant with block methods)
.   -ksp_hpddm_variant <type, default=left> - any of left, right, or flexible
.   -ksp_hpddm_recycle <n, default=0> - number of harmonic Ritz vectors to compute (only relevant with GCRODR or BGCRODR)
.   -ksp_hpddm_recycle_target <type, default=SM> - criterion to select harmonic Ritz vectors using either SM, LM, SR, LR, SI, or LI (only relevant with GCRODR or BGCRODR)
-   -ksp_hpddm_recycle_strategy <type, default=A> - generalized eigenvalue problem A or B to solve for recycling (only relevant with flexible GCRODR or BGCRODR)

   References:
+   1980 - The Block Conjugate Gradient Algorithm and Related Methods. O'Leary. Linear Algebra and its Applications.
.   2006 - Recycling Krylov Subspaces for Sequences of Linear Systems. Parks, de Sturler, Mackey, Johnson, and Maiti. SIAM Journal on Scientific Computing
.   2013 - A Modified Block Flexible GMRES Method with Deflation at Each Iteration for the Solution of Non-Hermitian Linear Systems with Multiple Right-Hand Sides. Calandra, Gratton, Lago, Vasseur, and Carvalho. SIAM Journal on Scientific Computing.
.   2016 - Block Iterative Methods and Recycling for Improved Scalability of Linear Solvers. Jolivet and Tournier. SC16.
-   2017 - A breakdown-free block conjugate gradient method. Ji and Li. BIT Numerical Mathematics.

   Level: intermediate

.seealso:  KSPCreate(), KSPSetType(), KSPType (for list of available types), KSP, KSPGMRES, KSPCG, KSPLGMRES, KSPDGMRES
M*/
PETSC_EXTERN PetscErrorCode KSPCreate_HPDDM(KSP ksp)
{
  PetscErrorCode ierr;

  PetscFunctionBegin;
  ierr = KSPSetSupportedNorm(ksp, KSP_NORM_PRECONDITIONED, PC_LEFT, 2);CHKERRQ(ierr);
  ierr = KSPSetSupportedNorm(ksp, KSP_NORM_UNPRECONDITIONED, PC_RIGHT, 1);CHKERRQ(ierr);
  ksp->ops->setup          = KSPSetUp_HPDDM;
  ksp->ops->solve          = KSPSolve_HPDDM;
  ksp->ops->reset          = KSPReset_HPDDM;
  ksp->ops->destroy        = KSPDestroy_HPDDM;
  ksp->ops->setfromoptions = KSPSetFromOptions_HPDDM;
  ierr = PetscObjectComposeFunction((PetscObject)ksp, "KSPHPDDMSetDeflationSpace_C", KSPHPDDMSetDeflationSpace_HPDDM);CHKERRQ(ierr);
  ierr = PetscObjectComposeFunction((PetscObject)ksp, "KSPHPDDMGetDeflationSpace_C", KSPHPDDMGetDeflationSpace_HPDDM);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}
