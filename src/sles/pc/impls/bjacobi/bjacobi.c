/*$Id: bjacobi.c,v 1.155 2001/02/05 16:51:57 bsmith Exp bsmith $*/
/*
   Defines a block Jacobi preconditioner.
*/
#include "src/mat/matimpl.h"
#include "src/sles/pc/pcimpl.h"              /*I "petscpc.h" I*/
#include "src/sles/pc/impls/bjacobi/bjacobi.h"

static int PCSetUp_BJacobi_Singleblock(PC,Mat,Mat);
static int PCSetUp_BJacobi_Multiblock(PC,Mat,Mat);

#undef __FUNC__  
#define __FUNC__ "PCSetUp_BJacobi"
static int PCSetUp_BJacobi(PC pc)
{
  PC_BJacobi      *jac = (PC_BJacobi*)pc->data;
  Mat             mat = pc->mat,pmat = pc->pmat;
  int             ierr,N,M,start,i,rank,size,sum,end;
  int             bs,i_start=-1,i_end=-1;
  char            *pprefix,*mprefix;

  PetscFunctionBegin;
  ierr = MPI_Comm_rank(pc->comm,&rank);CHKERRQ(ierr);
  ierr = MPI_Comm_size(pc->comm,&size);CHKERRQ(ierr);
  ierr = MatGetLocalSize(pc->pmat,&M,&N);CHKERRQ(ierr);
  ierr = MatGetBlockSize(pc->pmat,&bs);CHKERRQ(ierr);

  /* ----------
      Determines the number of blocks assigned to each processor 
  */

  /*   local block count  given */
  if (jac->n_local > 0 && jac->n < 0) {
    ierr = MPI_Allreduce(&jac->n_local,&jac->n,1,MPI_INT,MPI_SUM,pc->comm);CHKERRQ(ierr);
    if (jac->l_lens) { /* check that user set these correctly */
      sum = 0;
      for (i=0; i<jac->n_local; i++) {
        if (jac->l_lens[i]/bs*bs !=jac->l_lens[i]) {
          SETERRQ(PETSC_ERR_ARG_SIZ,"Mat blocksize doesn't match block Jacobi layout");
        }
        sum += jac->l_lens[i];
      }
      if (sum != M) SETERRQ(PETSC_ERR_ARG_SIZ,"Local lens sent incorrectly");
    }
  } else if (jac->n > 0 && jac->n_local < 0) { /* global block count given */
    /* global blocks given: determine which ones are local */
    if (jac->g_lens) {
      /* check if the g_lens is has valid entries */
      for (i=0; i<jac->n; i++) {
        if (!jac->g_lens[i]) SETERRQ(PETSC_ERR_ARG_SIZ,"Zero block not allowed");
        if (jac->g_lens[i]/bs*bs != jac->g_lens[i]) {
          SETERRQ(PETSC_ERR_ARG_SIZ,"Mat blocksize doesn't match block Jacobi layout");
        }
      }
      if (size == 1) {
        jac->n_local = jac->n;
        ierr         = PetscMalloc(jac->n_local*sizeof(int),&jac->l_lens);CHKERRQ(ierr);
        ierr         = PetscMemcpy(jac->l_lens,jac->g_lens,jac->n_local*sizeof(int));CHKERRQ(ierr);
        /* check that user set these correctly */
        sum = 0;
        for (i=0; i<jac->n_local; i++) sum += jac->l_lens[i];
        if (sum != M) SETERRQ(PETSC_ERR_ARG_SIZ,"Global lens sent incorrectly");
      } else {
        ierr = MatGetOwnershipRange(pc->pmat,&start,&end);CHKERRQ(ierr);
        /* loop over blocks determing first one owned by me */
        sum = 0;
        for (i=0; i<jac->n+1; i++) {
          if (sum == start) { i_start = i; goto start_1;}
          if (i < jac->n) sum += jac->g_lens[i];
        }
        SETERRQ(PETSC_ERR_ARG_SIZ,"Block sizes\n\
                   used in PCBJacobiSetTotalBlocks()\n\
                   are not compatible with parallel matrix layout");
 start_1: 
        for (i=i_start; i<jac->n+1; i++) {
          if (sum == end) { i_end = i; goto end_1; }
          if (i < jac->n) sum += jac->g_lens[i];
        }          
        SETERRQ(PETSC_ERR_ARG_SIZ,"Block sizes\n\
                      used in PCBJacobiSetTotalBlocks()\n\
                      are not compatible with parallel matrix layout");
 end_1: 
        jac->n_local = i_end - i_start;
        ierr         = PetscMalloc(jac->n_local*sizeof(int),&jac->l_lens);CHKERRQ(ierr); 
        ierr         = PetscMemcpy(jac->l_lens,jac->g_lens+i_start,jac->n_local*sizeof(int));CHKERRQ(ierr);
      }
    } else { /* no global blocks given, determine then using default layout */
      jac->n_local = jac->n/size + ((jac->n % size) > rank);
      ierr         = PetscMalloc(jac->n_local*sizeof(int),&jac->l_lens);CHKERRQ(ierr);
      for (i=0; i<jac->n_local; i++) {
        jac->l_lens[i] = ((M/bs)/jac->n_local + (((M/bs) % jac->n_local) > i))*bs;
        if (!jac->l_lens[i]) SETERRQ(PETSC_ERR_ARG_SIZ,"Too many blocks given");
      }
    }
  } else if (jac->n < 0 && jac->n_local < 0) { /* no blocks given */
    jac->n         = size;
    jac->n_local   = 1;
    ierr           = PetscMalloc(sizeof(int),&jac->l_lens);CHKERRQ(ierr);
    jac->l_lens[0] = M;
  }

  ierr = MPI_Comm_size(pc->comm,&size);CHKERRQ(ierr);
  if (size == 1) {
    mat  = pc->mat;
    pmat = pc->pmat;
  } else {
    PetscTruth iscopy;
    MatReuse   scall;
    int        (*f)(Mat,PetscTruth*,MatReuse,Mat*);

    if (jac->use_true_local) {
      scall = MAT_INITIAL_MATRIX;
      if (pc->setupcalled) {
        if (pc->flag == SAME_NONZERO_PATTERN) {
          if (jac->tp_mat) {
            scall = MAT_REUSE_MATRIX;
            mat   = jac->tp_mat;
          }
        } else {
          if (jac->tp_mat)  {
            ierr = MatDestroy(jac->tp_mat);CHKERRQ(ierr);
          }
        }
      }
      ierr = PetscObjectQueryFunction((PetscObject)pc->mat,"MatGetDiagonalBlock_C",(void**)&f);CHKERRQ(ierr);
      if (!f) {
        SETERRQ(PETSC_ERR_SUP,"This matrix does not support getting diagonal block");
      }
      ierr = (*f)(pc->mat,&iscopy,scall,&mat);CHKERRQ(ierr);
      /* make submatrix have same prefix as entire matrix */
      ierr = PetscObjectGetOptionsPrefix((PetscObject)pc->mat,&mprefix);CHKERRQ(ierr);
      ierr = PetscObjectSetOptionsPrefix((PetscObject)mat,mprefix);CHKERRQ(ierr);
      if (iscopy) {
        jac->tp_mat = mat;
      }
    }
    if (pc->pmat != pc->mat || !jac->use_true_local) {
      scall = MAT_INITIAL_MATRIX;
      if (pc->setupcalled) {
        if (pc->flag == SAME_NONZERO_PATTERN) {
          if (jac->tp_pmat) {
            scall = MAT_REUSE_MATRIX;
            pmat   = jac->tp_pmat;
          }
        } else {
          if (jac->tp_pmat)  {
            ierr = MatDestroy(jac->tp_pmat);CHKERRQ(ierr);
          }
        }
      }
      ierr = PetscObjectQueryFunction((PetscObject)pc->pmat,"MatGetDiagonalBlock_C",(void**)&f);CHKERRQ(ierr);
      if (!f) {
        SETERRQ(PETSC_ERR_SUP,"This matrix does not support getting diagonal block");
      }
      ierr = (*f)(pc->pmat,&iscopy,scall,&pmat);CHKERRQ(ierr);
      /* make submatrix have same prefix as entire matrix */
      ierr = PetscObjectGetOptionsPrefix((PetscObject)pc->pmat,&pprefix);CHKERRQ(ierr);
      ierr = PetscObjectSetOptionsPrefix((PetscObject)pmat,pprefix);CHKERRQ(ierr);
      if (iscopy) {
        jac->tp_pmat = pmat;
      }
    } else {
      pmat = mat;
    }
  }

  /* ------
     Setup code depends on the number of blocks 
  */
  if (jac->n_local == 1) {
    ierr = PCSetUp_BJacobi_Singleblock(pc,mat,pmat);CHKERRQ(ierr);
  } else {
    ierr = PCSetUp_BJacobi_Multiblock(pc,mat,pmat);CHKERRQ(ierr);
  }
  PetscFunctionReturn(0);
}

/* Default destroy, if it has never been setup */
#undef __FUNC__  
#define __FUNC__ "PCDestroy_BJacobi"
static int PCDestroy_BJacobi(PC pc)
{
  PC_BJacobi *jac = (PC_BJacobi*)pc->data;
  int ierr;

  PetscFunctionBegin;
  if (jac->g_lens) {ierr = PetscFree(jac->g_lens);CHKERRQ(ierr);}
  if (jac->l_lens) {ierr = PetscFree(jac->l_lens);CHKERRQ(ierr);}
  ierr = PetscFree(jac);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCSetFromOptions_BJacobi"
static int PCSetFromOptions_BJacobi(PC pc)
{
  PC_BJacobi *jac = (PC_BJacobi*)pc->data;
  int        blocks,ierr;
  PetscTruth flg;

  PetscFunctionBegin;
  ierr = PetscOptionsHead("Block Jacobi options");CHKERRQ(ierr);
    ierr = PetscOptionsInt("-pc_bjacobi_blocks","Total number of blocks","PCBJacobiSetTotalBlocks",jac->n,&blocks,&flg);CHKERRQ(ierr);
    if (flg) {
      ierr = PCBJacobiSetTotalBlocks(pc,blocks,PETSC_NULL);CHKERRQ(ierr); 
    }
    ierr = PetscOptionsName("-pc_bjacobi_truelocal","Use the true matrix, not preconditioner matrix to define matrix vector product in sub-problems","PCBJacobiSetUseTrueLocal",&flg);CHKERRQ(ierr);
    if (flg) {
      ierr = PCBJacobiSetUseTrueLocal(pc);CHKERRQ(ierr);
    }
  ierr = PetscOptionsTail();CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCView_BJacobi"
static int PCView_BJacobi(PC pc,PetscViewer viewer)
{
  PC_BJacobi *jac = (PC_BJacobi*)pc->data;
  int        rank,ierr,i;
  PetscTruth isascii,isstring;
  PetscViewer     sviewer;

  PetscFunctionBegin;
  ierr = PetscTypeCompare((PetscObject)viewer,PETSC_VIEWER_ASCII,&isascii);CHKERRQ(ierr);
  ierr = PetscTypeCompare((PetscObject)viewer,PETSC_VIEWER_STRING,&isstring);CHKERRQ(ierr);
  if (isascii) {
    if (jac->use_true_local) {
      ierr = PetscViewerASCIIPrintf(viewer,"  block Jacobi: using true local matrix, number of blocks = %d\n",jac->n);CHKERRQ(ierr);
    }
    ierr = PetscViewerASCIIPrintf(viewer,"  block Jacobi: number of blocks = %d\n",jac->n);CHKERRQ(ierr);
    ierr = MPI_Comm_rank(pc->comm,&rank);CHKERRQ(ierr);
    if (jac->same_local_solves) {
      ierr = PetscViewerASCIIPrintf(viewer,"  Local solve is same for all blocks, in the following KSP and PC objects:\n");CHKERRQ(ierr);
      ierr = PetscViewerGetSingleton(viewer,&sviewer);CHKERRQ(ierr);
      if (!rank && jac->sles) {
        ierr = PetscViewerASCIIPushTab(viewer);CHKERRQ(ierr);
        ierr = SLESView(jac->sles[0],sviewer);CHKERRQ(ierr);
        ierr = PetscViewerASCIIPopTab(viewer);CHKERRQ(ierr);
      }   
      ierr = PetscViewerRestoreSingleton(viewer,&sviewer);CHKERRQ(ierr);
    } else {

      ierr = PetscViewerASCIIPrintf(viewer,"  Local solve info for each block is in the following KSP and PC objects:\n");CHKERRQ(ierr);
      ierr = PetscViewerASCIISynchronizedPrintf(viewer,"Proc %d: number of local blocks = %d, first local block number = %d\n",
                   rank,jac->n_local,jac->first_local);CHKERRQ(ierr);
      ierr = PetscViewerASCIIPushTab(viewer);CHKERRQ(ierr);
      for (i=0; i<jac->n_local; i++) {
        ierr = PetscViewerASCIISynchronizedPrintf(viewer,"Proc %d: local block number %d\n",rank,i);CHKERRQ(ierr);
        ierr = PetscViewerGetSingleton(viewer,&sviewer);CHKERRQ(ierr);
        ierr = SLESView(jac->sles[i],sviewer);CHKERRQ(ierr);
        if (i != jac->n_local-1) {
          ierr = PetscViewerASCIISynchronizedPrintf(viewer,"- - - - - - - - - - - - - - - - - -\n");CHKERRQ(ierr);
        }
        ierr = PetscViewerRestoreSingleton(viewer,&sviewer);CHKERRQ(ierr);
      }
      ierr = PetscViewerASCIIPopTab(viewer);CHKERRQ(ierr);
      ierr = PetscViewerFlush(viewer);CHKERRQ(ierr);
    }
  } else if (isstring) {
    ierr = PetscViewerStringSPrintf(viewer," blks=%d",jac->n);CHKERRQ(ierr);
    ierr = PetscViewerGetSingleton(viewer,&sviewer);CHKERRQ(ierr);
    if (jac->sles) {ierr = SLESView(jac->sles[0],sviewer);CHKERRQ(ierr);}
    ierr = PetscViewerRestoreSingleton(viewer,&sviewer);CHKERRQ(ierr);
  } else {
    SETERRQ1(1,"Viewer type %s not supported for block Jacobi",((PetscObject)viewer)->type_name);
  }
  PetscFunctionReturn(0);
}

/* -------------------------------------------------------------------------------------*/  

EXTERN_C_BEGIN
#undef __FUNC__  
#define __FUNC__ "PCBJacobiSetUseTrueLocal_BJacobi"
int PCBJacobiSetUseTrueLocal_BJacobi(PC pc)
{
  PC_BJacobi   *jac;

  PetscFunctionBegin;
  jac                 = (PC_BJacobi*)pc->data;
  jac->use_true_local = PETSC_TRUE;
  PetscFunctionReturn(0);
}
EXTERN_C_END

EXTERN_C_BEGIN
#undef __FUNC__  
#define __FUNC__ "PCBJacobiGetSubSLES_BJacobi"
int PCBJacobiGetSubSLES_BJacobi(PC pc,int *n_local,int *first_local,SLES **sles)
{
  PC_BJacobi   *jac = (PC_BJacobi*)pc->data;;

  PetscFunctionBegin;
  if (!pc->setupcalled) SETERRQ(PETSC_ERR_ARG_WRONGSTATE,"Must call SLESSetUp() or PCSetUp() first");

  if (n_local)     *n_local     = jac->n_local;
  if (first_local) *first_local = jac->first_local;
  *sles                         = jac->sles;
  jac->same_local_solves        = PETSC_FALSE; /* Assume that local solves are now different;
                                                  not necessarily true though!  This flag is 
                                                  used only for PCView_BJacobi() */
  PetscFunctionReturn(0);
}
EXTERN_C_END

EXTERN_C_BEGIN
#undef __FUNC__  
#define __FUNC__ "PCBJacobiSetTotalBlocks_BJacobi"
int PCBJacobiSetTotalBlocks_BJacobi(PC pc,int blocks,int *lens)
{
  PC_BJacobi *jac = (PC_BJacobi*)pc->data; 
  int        ierr;

  PetscFunctionBegin;

  jac->n = blocks;
  if (!lens) {
    jac->g_lens = 0;
  } else {
    ierr = PetscMalloc(blocks*sizeof(int),&jac->g_lens);CHKERRQ(ierr);
    PetscLogObjectMemory(pc,blocks*sizeof(int));
    ierr = PetscMemcpy(jac->g_lens,lens,blocks*sizeof(int));CHKERRQ(ierr);
  }
  PetscFunctionReturn(0);
}
EXTERN_C_END

EXTERN_C_BEGIN
#undef __FUNC__  
#define __FUNC__ "PCBJacobiSetLocalBlocks_BJacobi"
int PCBJacobiSetLocalBlocks_BJacobi(PC pc,int blocks,int *lens)
{
  PC_BJacobi *jac;
  int        ierr;

  PetscFunctionBegin;
  jac = (PC_BJacobi*)pc->data; 

  jac->n_local = blocks;
  if (!lens) {
    jac->l_lens = 0;
  } else {
    ierr = PetscMalloc(blocks*sizeof(int),&jac->l_lens);CHKERRQ(ierr);
    PetscLogObjectMemory(pc,blocks*sizeof(int));
    ierr = PetscMemcpy(jac->l_lens,lens,blocks*sizeof(int));CHKERRQ(ierr);
  }
  PetscFunctionReturn(0);
}
EXTERN_C_END

/* -------------------------------------------------------------------------------------*/  

#undef __FUNC__  
#define __FUNC__ "PCBJacobiSetUseTrueLocal"
/*@
   PCBJacobiSetUseTrueLocal - Sets a flag to indicate that the block 
   problem is associated with the linear system matrix instead of the
   default (where it is associated with the preconditioning matrix).
   That is, if the local system is solved iteratively then it iterates
   on the block from the matrix using the block from the preconditioner
   as the preconditioner for the local block.

   Collective on PC

   Input Parameters:
.  pc - the preconditioner context

   Options Database Key:
.  -pc_bjacobi_truelocal - Activates PCBJacobiSetUseTrueLocal()

   Notes:
   For the common case in which the preconditioning and linear 
   system matrices are identical, this routine is unnecessary.

   Level: intermediate

.keywords:  block, Jacobi, set, true, local, flag

.seealso: PCSetOperators(), PCBJacobiSetLocalBlocks()
@*/
int PCBJacobiSetUseTrueLocal(PC pc)
{
  int ierr,(*f)(PC);

  PetscFunctionBegin;
  PetscValidHeaderSpecific(pc,PC_COOKIE);
  ierr = PetscObjectQueryFunction((PetscObject)pc,"PCBJacobiSetUseTrueLocal_C",(void **)&f);CHKERRQ(ierr);
  if (f) {
    ierr = (*f)(pc);CHKERRQ(ierr);
  } 

  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCBJacobiGetSubSLES"
/*@C
   PCBJacobiGetSubSLES - Gets the local SLES contexts for all blocks on
   this processor.
   
   Note Collective

   Input Parameter:
.  pc - the preconditioner context

   Output Parameters:
+  n_local - the number of blocks on this processor, or PETSC_NULL
.  first_local - the global number of the first block on this processor, or PETSC_NULL
-  sles - the array of SLES contexts

   Notes:  
   After PCBJacobiGetSubSLES() the array of SLES contexts is not to be freed.
   
   Currently for some matrix implementations only 1 block per processor 
   is supported.
   
   You must call SLESSetUp() or PCSetUp() before calling PCBJacobiGetSubSLES().

   Level: advanced

.keywords:  block, Jacobi, get, sub, SLES, context

.seealso: PCBJacobiGetSubSLES()
@*/
int PCBJacobiGetSubSLES(PC pc,int *n_local,int *first_local,SLES **sles)
{
  int ierr,(*f)(PC,int *,int *,SLES **);

  PetscFunctionBegin;
  PetscValidHeaderSpecific(pc,PC_COOKIE);
  ierr = PetscObjectQueryFunction((PetscObject)pc,"PCBJacobiGetSubSLES_C",(void **)&f);CHKERRQ(ierr);
  if (f) {
    ierr = (*f)(pc,n_local,first_local,sles);CHKERRQ(ierr);
  } else {
    SETERRQ(1,"Cannot get subsolvers for this preconditioner");
  }
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCBJacobiSetTotalBlocks"
/*@
   PCBJacobiSetTotalBlocks - Sets the global number of blocks for the block
   Jacobi preconditioner.

   Collective on PC

   Input Parameters:
+  pc - the preconditioner context
.  blocks - the number of blocks
-  lens - [optional] integer array containing the size of each block

   Options Database Key:
.  -pc_bjacobi_blocks <blocks> - Sets the number of global blocks

   Notes:  
   Currently only a limited number of blocking configurations are supported.
   All processors sharing the PC must call this routine with the same data.

   Level: intermediate

.keywords:  set, number, Jacobi, global, total, blocks

.seealso: PCBJacobiSetUseTrueLocal(), PCBJacobiSetLocalBlocks()
@*/
int PCBJacobiSetTotalBlocks(PC pc,int blocks,int *lens)
{
  int ierr,(*f)(PC,int,int *);

  PetscFunctionBegin;
  PetscValidHeaderSpecific(pc,PC_COOKIE);
  if (blocks <= 0) SETERRQ(PETSC_ERR_ARG_OUTOFRANGE,"Must have positive blocks");
  ierr = PetscObjectQueryFunction((PetscObject)pc,"PCBJacobiSetTotalBlocks_C",(void **)&f);CHKERRQ(ierr);
  if (f) {
    ierr = (*f)(pc,blocks,lens);CHKERRQ(ierr);
  } 
  PetscFunctionReturn(0);
}
  
#undef __FUNC__  
#define __FUNC__ "PCBJacobiSetLocalBlocks"
/*@
   PCBJacobiSetLocalBlocks - Sets the local number of blocks for the block
   Jacobi preconditioner.

   Not Collective

   Input Parameters:
+  pc - the preconditioner context
.  blocks - the number of blocks
-  lens - [optional] integer array containing size of each block

   Note:  
   Currently only a limited number of blocking configurations are supported.

   Level: intermediate

.keywords: PC, set, number, Jacobi, local, blocks

.seealso: PCBJacobiSetUseTrueLocal(), PCBJacobiSetTotalBlocks()
@*/
int PCBJacobiSetLocalBlocks(PC pc,int blocks,int *lens)
{
  int ierr,(*f)(PC,int,int *);

  PetscFunctionBegin;
  PetscValidHeaderSpecific(pc,PC_COOKIE);
  if (blocks < 0) SETERRQ(PETSC_ERR_ARG_OUTOFRANGE,"Must have nonegative blocks");
  ierr = PetscObjectQueryFunction((PetscObject)pc,"PCBJacobiSetLocalBlocks_C",(void **)&f);CHKERRQ(ierr);
  if (f) {
    ierr = (*f)(pc,blocks,lens);CHKERRQ(ierr);
  } 
  PetscFunctionReturn(0);
}

/* -----------------------------------------------------------------------------------*/

EXTERN_C_BEGIN
#undef __FUNC__  
#define __FUNC__ "PCCreate_BJacobi"
int PCCreate_BJacobi(PC pc)
{
  int          rank,ierr;
  PC_BJacobi   *jac;

  PetscFunctionBegin;
  ierr = PetscNew(PC_BJacobi,&jac);CHKERRQ(ierr);
  PetscLogObjectMemory(pc,sizeof(PC_BJacobi));
  ierr = MPI_Comm_rank(pc->comm,&rank);CHKERRQ(ierr);
  pc->ops->apply              = 0;
  pc->ops->applytranspose     = 0;
  pc->ops->setup              = PCSetUp_BJacobi;
  pc->ops->destroy            = PCDestroy_BJacobi;
  pc->ops->setfromoptions     = PCSetFromOptions_BJacobi;
  pc->ops->view               = PCView_BJacobi;
  pc->ops->applyrichardson    = 0;

  pc->data               = (void*)jac;
  jac->n                 = -1;
  jac->n_local           = -1;
  jac->first_local       = rank;
  jac->sles              = 0;
  jac->use_true_local    = PETSC_FALSE;
  jac->same_local_solves = PETSC_TRUE;
  jac->g_lens            = 0;
  jac->l_lens            = 0;
  jac->tp_mat            = 0;
  jac->tp_pmat           = 0;

  ierr = PetscObjectComposeFunctionDynamic((PetscObject)pc,"PCBJacobiSetUseTrueLocal_C",
                    "PCBJacobiSetUseTrueLocal_BJacobi",
                    PCBJacobiSetUseTrueLocal_BJacobi);CHKERRQ(ierr);
  ierr = PetscObjectComposeFunctionDynamic((PetscObject)pc,"PCBJacobiGetSubSLES_C","PCBJacobiGetSubSLES_BJacobi",
                    PCBJacobiGetSubSLES_BJacobi);CHKERRQ(ierr);
  ierr = PetscObjectComposeFunctionDynamic((PetscObject)pc,"PCBJacobiSetTotalBlocks_C","PCBJacobiSetTotalBlocks_BJacobi",
                    PCBJacobiSetTotalBlocks_BJacobi);CHKERRQ(ierr);
  ierr = PetscObjectComposeFunctionDynamic((PetscObject)pc,"PCBJacobiSetLocalBlocks_C","PCBJacobiSetLocalBlocks_BJacobi",
                    PCBJacobiSetLocalBlocks_BJacobi);CHKERRQ(ierr);

  PetscFunctionReturn(0);
}
EXTERN_C_END

/* --------------------------------------------------------------------------------------------*/
/*
        These are for a single block per processor; works for AIJ, BAIJ; Seq and MPI
*/
#undef __FUNC__  
#define __FUNC__ "PCDestroy_BJacobi_Singleblock"
int PCDestroy_BJacobi_Singleblock(PC pc)
{
  PC_BJacobi             *jac = (PC_BJacobi*)pc->data;
  PC_BJacobi_Singleblock *bjac = (PC_BJacobi_Singleblock*)jac->data;
  int                    ierr;

  PetscFunctionBegin;
  /*
        If the on processor block had to be generated via a MatGetDiagonalBlock()
     that creates a copy (for example MPIBDiag matrices do), this frees the space
  */
  if (jac->tp_mat) {
    ierr = MatDestroy(jac->tp_mat);CHKERRQ(ierr);
  }
  if (jac->tp_pmat) {
    ierr = MatDestroy(jac->tp_pmat);CHKERRQ(ierr);
  }

  ierr = SLESDestroy(jac->sles[0]);CHKERRQ(ierr);
  ierr = PetscFree(jac->sles);CHKERRQ(ierr);
  ierr = VecDestroy(bjac->x);CHKERRQ(ierr);
  ierr = VecDestroy(bjac->y);CHKERRQ(ierr);
  if (jac->l_lens) {ierr = PetscFree(jac->l_lens);CHKERRQ(ierr);}
  if (jac->g_lens) {ierr = PetscFree(jac->g_lens);CHKERRQ(ierr);}
  ierr = PetscFree(bjac);CHKERRQ(ierr);
  ierr = PetscFree(jac);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCSetUpOnBlocks_BJacobi_Singleblock"
int PCSetUpOnBlocks_BJacobi_Singleblock(PC pc)
{
  int                    ierr;
  PC_BJacobi             *jac = (PC_BJacobi*)pc->data;
  PC_BJacobi_Singleblock *bjac = (PC_BJacobi_Singleblock*)jac->data;

  PetscFunctionBegin;
  ierr = SLESSetUp(jac->sles[0],bjac->x,bjac->y);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCApply_BJacobi_Singleblock"
int PCApply_BJacobi_Singleblock(PC pc,Vec x,Vec y)
{
  int                    ierr,its;
  PC_BJacobi             *jac = (PC_BJacobi*)pc->data;
  PC_BJacobi_Singleblock *bjac = (PC_BJacobi_Singleblock*)jac->data;
  Scalar                 *x_array,*y_array;

  PetscFunctionBegin;
  /* 
      The VecPlaceArray() is to avoid having to copy the 
    y vector into the bjac->x vector. The reason for 
    the bjac->x vector is that we need a sequential vector
    for the sequential solve.
  */
  ierr = VecGetArray(x,&x_array);CHKERRQ(ierr); 
  ierr = VecGetArray(y,&y_array);CHKERRQ(ierr); 
  ierr = VecPlaceArray(bjac->x,x_array);CHKERRQ(ierr); 
  ierr = VecPlaceArray(bjac->y,y_array);CHKERRQ(ierr); 
  ierr = SLESSolve(jac->sles[0],bjac->x,bjac->y,&its);CHKERRQ(ierr); 
  ierr = VecRestoreArray(x,&x_array);CHKERRQ(ierr); 
  ierr = VecRestoreArray(y,&y_array);CHKERRQ(ierr); 
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCApplySymmetricLeft_BJacobi_Singleblock"
int PCApplySymmetricLeft_BJacobi_Singleblock(PC pc,Vec x,Vec y)
{
  int                    ierr;
  PC_BJacobi             *jac = (PC_BJacobi*)pc->data;
  PC_BJacobi_Singleblock *bjac = (PC_BJacobi_Singleblock*)jac->data;
  Scalar                 *x_array,*y_array;
  PC                     subpc;

  PetscFunctionBegin;
  /* 
      The VecPlaceArray() is to avoid having to copy the 
    y vector into the bjac->x vector. The reason for 
    the bjac->x vector is that we need a sequential vector
    for the sequential solve.
  */
  ierr = VecGetArray(x,&x_array);CHKERRQ(ierr); 
  ierr = VecGetArray(y,&y_array);CHKERRQ(ierr); 
  ierr = VecPlaceArray(bjac->x,x_array);CHKERRQ(ierr); 
  ierr = VecPlaceArray(bjac->y,y_array);CHKERRQ(ierr); 

  /* apply the symmetric left portion of the inner PC operator */
  /* note this by-passes the inner SLES and its options completely */

  ierr = SLESGetPC(jac->sles[0],&subpc);CHKERRQ(ierr);
  ierr = PCApplySymmetricLeft(subpc,bjac->x,bjac->y);CHKERRQ(ierr);

  ierr = VecRestoreArray(x,&x_array);CHKERRQ(ierr); 
  ierr = VecRestoreArray(y,&y_array);CHKERRQ(ierr); 
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCApplySymmetricRight_BJacobi_Singleblock"
int PCApplySymmetricRight_BJacobi_Singleblock(PC pc,Vec x,Vec y)
{
  int                    ierr;
  PC_BJacobi             *jac = (PC_BJacobi*)pc->data;
  PC_BJacobi_Singleblock *bjac = (PC_BJacobi_Singleblock*)jac->data;
  Scalar                 *x_array,*y_array;
  PC                     subpc;

  PetscFunctionBegin;
  /* 
      The VecPlaceArray() is to avoid having to copy the 
    y vector into the bjac->x vector. The reason for 
    the bjac->x vector is that we need a sequential vector
    for the sequential solve.
  */
  ierr = VecGetArray(x,&x_array);CHKERRQ(ierr); 
  ierr = VecGetArray(y,&y_array);CHKERRQ(ierr); 
  ierr = VecPlaceArray(bjac->x,x_array);CHKERRQ(ierr); 
  ierr = VecPlaceArray(bjac->y,y_array);CHKERRQ(ierr); 

  /* apply the symmetric right portion of the inner PC operator */
  /* note this by-passes the inner SLES and its options completely */

  ierr = SLESGetPC(jac->sles[0],&subpc);CHKERRQ(ierr);
  ierr = PCApplySymmetricRight(subpc,bjac->x,bjac->y);CHKERRQ(ierr);

  ierr = VecRestoreArray(x,&x_array);CHKERRQ(ierr); 
  ierr = VecRestoreArray(y,&y_array);CHKERRQ(ierr); 
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCApplyTranspose_BJacobi_Singleblock"
int PCApplyTranspose_BJacobi_Singleblock(PC pc,Vec x,Vec y)
{
  int                    ierr,its;
  PC_BJacobi             *jac = (PC_BJacobi*)pc->data;
  PC_BJacobi_Singleblock *bjac = (PC_BJacobi_Singleblock*)jac->data;
  Scalar                 *x_array,*y_array;

  PetscFunctionBegin;
  /* 
      The VecPlaceArray() is to avoid having to copy the 
    y vector into the bjac->x vector. The reason for 
    the bjac->x vector is that we need a sequential vector
    for the sequential solve.
  */
  ierr = VecGetArray(x,&x_array);CHKERRQ(ierr); 
  ierr = VecGetArray(y,&y_array);CHKERRQ(ierr); 
  ierr = VecPlaceArray(bjac->x,x_array);CHKERRQ(ierr); 
  ierr = VecPlaceArray(bjac->y,y_array);CHKERRQ(ierr); 
  ierr = SLESSolveTranspose(jac->sles[0],bjac->x,bjac->y,&its);CHKERRQ(ierr); 
  ierr = VecRestoreArray(x,&x_array);CHKERRQ(ierr); 
  ierr = VecRestoreArray(y,&y_array);CHKERRQ(ierr); 
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCSetUp_BJacobi_Singleblock"
static int PCSetUp_BJacobi_Singleblock(PC pc,Mat mat,Mat pmat)
{
  PC_BJacobi             *jac = (PC_BJacobi*)pc->data;
  int                    ierr,m;
  SLES                   sles;
  Vec                    x,y;
  PC_BJacobi_Singleblock *bjac;
  KSP                    subksp;
  PC                     subpc;

  PetscFunctionBegin;

  /* set default direct solver with no Krylov method */
  if (!pc->setupcalled) {
    char *prefix;
    ierr = SLESCreate(PETSC_COMM_SELF,&sles);CHKERRQ(ierr);
    PetscLogObjectParent(pc,sles);
    ierr = SLESGetKSP(sles,&subksp);CHKERRQ(ierr);
    ierr = KSPSetType(subksp,KSPPREONLY);CHKERRQ(ierr);
    ierr = SLESGetPC(sles,&subpc);CHKERRQ(ierr);
    ierr = PCSetType(subpc,PCILU);CHKERRQ(ierr);
    ierr = PCGetOptionsPrefix(pc,&prefix);CHKERRQ(ierr);
    ierr = SLESSetOptionsPrefix(sles,prefix);CHKERRQ(ierr);
    ierr = SLESAppendOptionsPrefix(sles,"sub_");CHKERRQ(ierr);
    ierr = SLESSetFromOptions(sles);CHKERRQ(ierr);
    /*
      The reason we need to generate these vectors is to serve 
      as the right-hand side and solution vector for the solve on the 
      block. We do not need to allocate space for the vectors since
      that is provided via VecPlaceArray() just before the call to 
      SLESSolve() on the block.
    */
    ierr = MatGetSize(pmat,&m,&m);CHKERRQ(ierr);
    ierr = VecCreateSeqWithArray(PETSC_COMM_SELF,m,PETSC_NULL,&x);CHKERRQ(ierr);
    ierr = VecCreateSeqWithArray(PETSC_COMM_SELF,m,PETSC_NULL,&y);CHKERRQ(ierr);
    PetscLogObjectParent(pc,x);
    PetscLogObjectParent(pc,y);

    pc->ops->destroy             = PCDestroy_BJacobi_Singleblock;
    pc->ops->apply               = PCApply_BJacobi_Singleblock;
    pc->ops->applysymmetricleft  = PCApplySymmetricLeft_BJacobi_Singleblock;
    pc->ops->applysymmetricright = PCApplySymmetricRight_BJacobi_Singleblock;
    pc->ops->applytranspose      = PCApplyTranspose_BJacobi_Singleblock;
    pc->ops->setuponblocks       = PCSetUpOnBlocks_BJacobi_Singleblock;

    ierr = PetscMalloc(sizeof(PC_BJacobi_Singleblock),&bjac);CHKERRQ(ierr);
    PetscLogObjectMemory(pc,sizeof(PC_BJacobi_Singleblock));
    bjac->x      = x;
    bjac->y      = y;

    ierr = PetscMalloc(sizeof(SLES),&jac->sles);CHKERRQ(ierr);
    jac->sles[0] = sles;
    jac->data    = (void*)bjac;
  } else {
    sles = jac->sles[0];
    bjac = (PC_BJacobi_Singleblock *)jac->data;
  }
  if (jac->use_true_local) {
    ierr = SLESSetOperators(sles,mat,pmat,pc->flag);CHKERRQ(ierr);
  }  else {
    ierr = SLESSetOperators(sles,pmat,pmat,pc->flag);CHKERRQ(ierr);
  }   
  PetscFunctionReturn(0);
}

/* ---------------------------------------------------------------------------------------------*/

#undef __FUNC__  
#define __FUNC__ "PCDestroy_BJacobi_Multiblock"
int PCDestroy_BJacobi_Multiblock(PC pc)
{
  PC_BJacobi            *jac = (PC_BJacobi*)pc->data;
  PC_BJacobi_Multiblock *bjac = (PC_BJacobi_Multiblock*)jac->data;
  int                   i,ierr;

  PetscFunctionBegin;
  ierr = MatDestroyMatrices(jac->n_local,&bjac->pmat);CHKERRQ(ierr);
  if (jac->use_true_local) {
    ierr = MatDestroyMatrices(jac->n_local,&bjac->mat);CHKERRQ(ierr);
  }

  /*
        If the on processor block had to be generated via a MatGetDiagonalBlock()
     that creates a copy (for example MPIBDiag matrices do), this frees the space
  */
  if (jac->tp_mat) {
    ierr = MatDestroy(jac->tp_mat);CHKERRQ(ierr);
  }
  if (jac->tp_pmat) {
    ierr = MatDestroy(jac->tp_pmat);CHKERRQ(ierr);
  }

  for (i=0; i<jac->n_local; i++) {
    ierr = SLESDestroy(jac->sles[i]);CHKERRQ(ierr);
    ierr = VecDestroy(bjac->x[i]);CHKERRQ(ierr);
    ierr = VecDestroy(bjac->y[i]);CHKERRQ(ierr);
    ierr = ISDestroy(bjac->is[i]);CHKERRQ(ierr);
  }
  ierr = PetscFree(jac->sles);CHKERRQ(ierr);
  ierr = PetscFree(bjac->x);CHKERRQ(ierr);
  ierr = PetscFree(bjac->starts);CHKERRQ(ierr);
  ierr = PetscFree(bjac->is);CHKERRQ(ierr);
  ierr = PetscFree(bjac);CHKERRQ(ierr);
  if (jac->l_lens) {ierr = PetscFree(jac->l_lens);CHKERRQ(ierr);}
  if (jac->g_lens) {ierr = PetscFree(jac->g_lens);CHKERRQ(ierr);}
  ierr = PetscFree(jac);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCSetUpOnBlocks_BJacobi_Multiblock"
int PCSetUpOnBlocks_BJacobi_Multiblock(PC pc)
{
  PC_BJacobi            *jac = (PC_BJacobi*)pc->data;
  int                   ierr,i,n_local = jac->n_local;
  PC_BJacobi_Multiblock *bjac = (PC_BJacobi_Multiblock*)jac->data;

  PetscFunctionBegin;
  for (i=0; i<n_local; i++) {
    ierr = SLESSetUp(jac->sles[i],bjac->x[i],bjac->y[i]);CHKERRQ(ierr);
  }
  PetscFunctionReturn(0);
}

/*
      Preconditioner for block Jacobi 
*/
#undef __FUNC__  
#define __FUNC__ "PCApply_BJacobi_Multiblock"
int PCApply_BJacobi_Multiblock(PC pc,Vec x,Vec y)
{
  PC_BJacobi            *jac = (PC_BJacobi*)pc->data;
  int                   ierr,its,i,n_local = jac->n_local;
  PC_BJacobi_Multiblock *bjac = (PC_BJacobi_Multiblock*)jac->data;
  Scalar                *xin,*yin;
  static PetscTruth     flag = PETSC_TRUE;
  static int            SUBSlesSolve;

  PetscFunctionBegin;
  if (flag) {
    ierr = PetscLogEventRegister(&SUBSlesSolve,"SubSlesSolve","black:");CHKERRQ(ierr);
    flag = PETSC_FALSE;
  }
  ierr = VecGetArray(x,&xin);CHKERRQ(ierr);
  ierr = VecGetArray(y,&yin);CHKERRQ(ierr);
  for (i=0; i<n_local; i++) {
    /* 
       To avoid copying the subvector from x into a workspace we instead 
       make the workspace vector array point to the subpart of the array of
       the global vector.
    */
    ierr = VecPlaceArray(bjac->x[i],xin+bjac->starts[i]);CHKERRQ(ierr);
    ierr = VecPlaceArray(bjac->y[i],yin+bjac->starts[i]);CHKERRQ(ierr);

    ierr = PetscLogEventBegin(SUBSlesSolve,jac->sles[i],bjac->x[i],bjac->y[i],0);CHKERRQ(ierr);
    ierr = SLESSolve(jac->sles[i],bjac->x[i],bjac->y[i],&its);CHKERRQ(ierr);
    ierr = PetscLogEventEnd(SUBSlesSolve,jac->sles[i],bjac->x[i],bjac->y[i],0);CHKERRQ(ierr);
  }
  ierr = VecRestoreArray(x,&xin);CHKERRQ(ierr);
  ierr = VecRestoreArray(y,&yin);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

/*
      Preconditioner for block Jacobi 
*/
#undef __FUNC__  
#define __FUNC__ "PCApplyTranspose_BJacobi_Multiblock"
int PCApplyTranspose_BJacobi_Multiblock(PC pc,Vec x,Vec y)
{
  PC_BJacobi            *jac = (PC_BJacobi*)pc->data;
  int                   ierr,its,i,n_local = jac->n_local;
  PC_BJacobi_Multiblock *bjac = (PC_BJacobi_Multiblock*)jac->data;
  Scalar                *xin,*yin;
  static PetscTruth     flag = PETSC_TRUE;
  static int            SUBSlesSolve;

  PetscFunctionBegin;
  if (flag) {
    ierr = PetscLogEventRegister(&SUBSlesSolve,"SubSlesSolveTranspose","black:");CHKERRQ(ierr);
    flag = PETSC_FALSE;
  }
  ierr = VecGetArray(x,&xin);CHKERRQ(ierr);
  ierr = VecGetArray(y,&yin);CHKERRQ(ierr);
  for (i=0; i<n_local; i++) {
    /* 
       To avoid copying the subvector from x into a workspace we instead 
       make the workspace vector array point to the subpart of the array of
       the global vector.
    */
    ierr = VecPlaceArray(bjac->x[i],xin+bjac->starts[i]);CHKERRQ(ierr);
    ierr = VecPlaceArray(bjac->y[i],yin+bjac->starts[i]);CHKERRQ(ierr);

    ierr = PetscLogEventBegin(SUBSlesSolve,jac->sles[i],bjac->x[i],bjac->y[i],0);CHKERRQ(ierr);
    ierr = SLESSolveTranspose(jac->sles[i],bjac->x[i],bjac->y[i],&its);CHKERRQ(ierr);
    ierr = PetscLogEventEnd(SUBSlesSolve,jac->sles[i],bjac->x[i],bjac->y[i],0);CHKERRQ(ierr);
  }
  ierr = VecRestoreArray(x,&xin);CHKERRQ(ierr);
  ierr = VecRestoreArray(y,&yin);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PCSetUp_BJacobi_Multiblock"
static int PCSetUp_BJacobi_Multiblock(PC pc,Mat mat,Mat pmat)
{
  PC_BJacobi             *jac = (PC_BJacobi*)pc->data;
  int                    ierr,m,n_local,N,M,start,i;
  char                   *prefix,*pprefix,*mprefix;
  SLES                   sles;
  Vec                    x,y;
  PC_BJacobi_Multiblock  *bjac = (PC_BJacobi_Multiblock*)jac->data;
  KSP                    subksp;
  PC                     subpc;
  IS                     is;
  MatReuse               scall = MAT_REUSE_MATRIX;

  PetscFunctionBegin;
  ierr = MatGetLocalSize(pc->pmat,&M,&N);CHKERRQ(ierr);

  n_local = jac->n_local;

  if (jac->use_true_local) {
    if (mat->type != pmat->type) SETERRQ(PETSC_ERR_ARG_INCOMP,"Matrices not of same type");
  }

  /* set default direct solver with no Krylov method */
  if (!pc->setupcalled) {
    scall                  = MAT_INITIAL_MATRIX;
    pc->ops->destroy       = PCDestroy_BJacobi_Multiblock;
    pc->ops->apply         = PCApply_BJacobi_Multiblock;
    pc->ops->applytranspose= PCApplyTranspose_BJacobi_Multiblock;
    pc->ops->setuponblocks = PCSetUpOnBlocks_BJacobi_Multiblock;

    ierr = PetscMalloc(sizeof(PC_BJacobi_Multiblock),&bjac);CHKERRQ(ierr);
    PetscLogObjectMemory(pc,sizeof(PC_BJacobi_Multiblock));
    ierr = PetscMalloc(n_local*sizeof(SLES),&jac->sles);CHKERRQ(ierr);
    PetscLogObjectMemory(pc,sizeof(n_local*sizeof(SLES)));
    ierr = PetscMalloc(2*n_local*sizeof(Vec),&bjac->x);CHKERRQ(ierr);
    PetscLogObjectMemory(pc,sizeof(2*n_local*sizeof(Vec)));
    bjac->y      = bjac->x + n_local;
    ierr = PetscMalloc(n_local*sizeof(Scalar),&bjac->starts);CHKERRQ(ierr);
    PetscLogObjectMemory(pc,sizeof(n_local*sizeof(Scalar)));
    
    jac->data    = (void*)bjac;
    ierr = PetscMalloc(n_local*sizeof(IS),&bjac->is);CHKERRQ(ierr);
    PetscLogObjectMemory(pc,sizeof(n_local*sizeof(IS)));

    start = 0;
    for (i=0; i<n_local; i++) {
      ierr = SLESCreate(PETSC_COMM_SELF,&sles);CHKERRQ(ierr);
      PetscLogObjectParent(pc,sles);
      ierr = SLESGetKSP(sles,&subksp);CHKERRQ(ierr);
      ierr = KSPSetType(subksp,KSPPREONLY);CHKERRQ(ierr);
      ierr = SLESGetPC(sles,&subpc);CHKERRQ(ierr);
      ierr = PCSetType(subpc,PCILU);CHKERRQ(ierr);
      ierr = PCGetOptionsPrefix(pc,&prefix);CHKERRQ(ierr);
      ierr = SLESSetOptionsPrefix(sles,prefix);CHKERRQ(ierr);
      ierr = SLESAppendOptionsPrefix(sles,"sub_");CHKERRQ(ierr);
      ierr = SLESSetFromOptions(sles);CHKERRQ(ierr);

      m = jac->l_lens[i];

      /*
      The reason we need to generate these vectors is to serve 
      as the right-hand side and solution vector for the solve on the 
      block. We do not need to allocate space for the vectors since
      that is provided via VecPlaceArray() just before the call to 
      SLESSolve() on the block.

      */
      ierr = VecCreateSeq(PETSC_COMM_SELF,m,&x);CHKERRQ(ierr);
      ierr = VecCreateSeqWithArray(PETSC_COMM_SELF,m,PETSC_NULL,&y);CHKERRQ(ierr);
      PetscLogObjectParent(pc,x);
      PetscLogObjectParent(pc,y);
      bjac->x[i]      = x;
      bjac->y[i]      = y;
      bjac->starts[i] = start;
      jac->sles[i]    = sles;

      ierr = ISCreateStride(PETSC_COMM_SELF,m,start,1,&is);CHKERRQ(ierr);
      bjac->is[i] = is;
      PetscLogObjectParent(pc,is);

      start += m;
    }
  } else {
    bjac = (PC_BJacobi_Multiblock*)jac->data;
    /* 
       Destroy the blocks from the previous iteration
    */
    if (pc->flag == DIFFERENT_NONZERO_PATTERN) {
      ierr = MatDestroyMatrices(n_local,&bjac->pmat);CHKERRQ(ierr);
      if (jac->use_true_local) {
        ierr = MatDestroyMatrices(n_local,&bjac->mat);CHKERRQ(ierr);
      }
      scall = MAT_INITIAL_MATRIX;
    }
  }

  ierr = MatGetSubMatrices(pmat,n_local,bjac->is,bjac->is,scall,&bjac->pmat);CHKERRQ(ierr);
  if (jac->use_true_local) {
    ierr = PetscObjectGetOptionsPrefix((PetscObject)mat,&mprefix);CHKERRQ(ierr);
    ierr = MatGetSubMatrices(mat,n_local,bjac->is,bjac->is,scall,&bjac->mat);CHKERRQ(ierr);
  }
  /* Return control to the user so that the submatrices can be modified (e.g., to apply
     different boundary conditions for the submatrices than for the global problem) */
  ierr = PCModifySubMatrices(pc,n_local,bjac->is,bjac->is,bjac->pmat,pc->modifysubmatricesP);CHKERRQ(ierr);

  ierr = PetscObjectGetOptionsPrefix((PetscObject)pmat,&pprefix);CHKERRQ(ierr);
  for (i=0; i<n_local; i++) {
    PetscLogObjectParent(pc,bjac->pmat[i]);
    ierr = PetscObjectSetOptionsPrefix((PetscObject)bjac->pmat[i],pprefix);CHKERRQ(ierr);
    if (jac->use_true_local) {
      PetscLogObjectParent(pc,bjac->mat[i]);
      ierr = PetscObjectSetOptionsPrefix((PetscObject)bjac->mat[i],mprefix);CHKERRQ(ierr);
      ierr = SLESSetOperators(jac->sles[i],bjac->mat[i],bjac->pmat[i],pc->flag);CHKERRQ(ierr);
    } else {
      ierr = SLESSetOperators(jac->sles[i],bjac->pmat[i],bjac->pmat[i],pc->flag);CHKERRQ(ierr);
    }
  }

  PetscFunctionReturn(0);
}










