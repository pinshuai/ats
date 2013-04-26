/*
  This is the flow component of the Amanzi code.
  License: BSD
  Authors: Konstantin Lipnikov (version 2) (lipnikov@lanl.gov)
*/

#include "errors.hh"
#include "Epetra_FECrsGraph.h"
#include "EpetraExt_RowMatrixOut.h"
#include "matrix_mfd.hh"
#include "matrix_mfd_tpfa.hh"

namespace Amanzi {
namespace Operators {

MatrixMFD::MatrixMFD(Teuchos::ParameterList& plist,
                     const Teuchos::RCP<const AmanziMesh::Mesh> mesh) :
    plist_(plist),
    mesh_(mesh),
    flag_symmetry_(false) {
  InitializeFromPList_();
}


MatrixMFD::MatrixMFD(const MatrixMFD& other) :
    plist_(other.plist_),
    mesh_(other.mesh_) {
  InitializeFromPList_();
}


void MatrixMFD::InitializeFromPList_() {
  std::string methodstring = plist_.get<string>("MFD method");
  method_ = MFD3D_NULL;

  // standard MFD
  if (methodstring == "polyhedra") {
    method_ = MFD3D_POLYHEDRA;
  } else if (methodstring == "polyhedra scaled") {
    method_ = MFD3D_POLYHEDRA_SCALED;
  } else if (methodstring == "optimized") {
    method_ = MFD3D_OPTIMIZED;
  } else if (methodstring == "optimized scaled") {
    method_ = MFD3D_OPTIMIZED_SCALED;
  } else if (methodstring == "hexahedra monotone") {
    method_ = MFD3D_HEXAHEDRA_MONOTONE;
  } else if (methodstring == "two point flux") {
    method_ = MFD3D_TWO_POINT_FLUX;
  } else if (methodstring == "support operator") {
    method_ = MFD3D_SUPPORT_OPERATOR;
  } else {
	Errors::Message msg("MatrixMFD: unexpected discretization methods");
	Exceptions::amanzi_throw(msg);
  }

  // method for inversion
  prec_method_ = PREC_METHOD_NULL;
  if (plist_.isParameter("preconditioner")) {
    std::string precmethodstring = plist_.get<string>("preconditioner");
    if (precmethodstring == "ML") {
      prec_method_ = TRILINOS_ML;
    } else if (precmethodstring == "ILU" ) {
      prec_method_ = TRILINOS_ILU;
    } else if (precmethodstring == "Block ILU" ) {
      prec_method_ = TRILINOS_BLOCK_ILU;
#ifdef HAVE_HYPRE
    } else if (precmethodstring == "HYPRE AMG") {
      prec_method_ = HYPRE_AMG;
    } else if (precmethodstring == "HYPRE Euclid") {
      prec_method_ = HYPRE_EUCLID;
    } else if (precmethodstring == "HYPRE ParaSails") {
      prec_method_ = HYPRE_EUCLID;
#endif
    } else {
#ifdef HAVE_HYPRE
      Errors::Message msg("Matrix_MFD: The specified preconditioner "+precmethodstring+" is not supported, we only support ML, ILU, HYPRE AMG, HYPRE Euclid, and HYPRE ParaSails");
#else
      Errors::Message msg("Matrix_MFD: The specified preconditioner "+precmethodstring+" is not supported, we only support ML, and ILU");
#endif
      Exceptions::amanzi_throw(msg);
    }
  }
}

// main computational methods
/* ******************************************************************
 * Calculate elemental inverse mass matrices.
 * WARNING: The original Aff_ matrices are destroyed.
 ****************************************************************** */
void MatrixMFD::CreateMFDmassMatrices(const Teuchos::Ptr<std::vector<WhetStone::Tensor> >& K) {
  int dim = mesh_->space_dimension();

  WhetStone::MFD3D mfd(mesh_);
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  Mff_cells_.clear();

  int ok;
  nokay_ = npassed_ = 0;

  WhetStone::Tensor Kc;
  if (K == Teuchos::null) {
    Kc.init(mesh_->space_dimension(), 1);
    Kc(0,0) = 1.0;
  }

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  for (int c=0; c!=ncells; ++c) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    Teuchos::SerialDenseMatrix<int, double> Mff(nfaces, nfaces);

    if (K != Teuchos::null) {
      Kc = (*K)[c];
    }

    if (method_ == MFD3D_POLYHEDRA_SCALED) {
      ok = mfd.DarcyMassInverseScaled(c, Kc, Mff);
    } else if (method_ == MFD3D_POLYHEDRA) {
      ok = mfd.DarcyMassInverse(c, Kc, Mff);
    } else if (method_ == MFD3D_OPTIMIZED_SCALED) {
      ok = mfd.DarcyMassInverseOptimizedScaled(c, Kc, Mff);
    } else if (method_ == MFD3D_OPTIMIZED) {
      ok = mfd.DarcyMassInverseOptimized(c, Kc, Mff);
    } else if (method_ == MFD3D_HEXAHEDRA_MONOTONE) {
      if ((nfaces == 6 && dim == 3) || (nfaces == 4 && dim == 2))
        ok = mfd.DarcyMassInverseHex(c, Kc, Mff);
      else
        ok = mfd.DarcyMassInverse(c, Kc, Mff);
    } else if (method_ == MFD3D_TWO_POINT_FLUX) {
      ok = mfd.DarcyMassInverseDiagonal(c, Kc, Mff);
    } else if (method_ == MFD3D_SUPPORT_OPERATOR) {
      ok = mfd.DarcyMassInverseSO(c, Kc, Mff);
    } else {
      Errors::Message msg("Flow PK: unexpected discretization methods (contact lipnikov@lanl.gov).");
      Exceptions::amanzi_throw(msg);
    }

    Mff_cells_.push_back(Mff);

    if (ok == WhetStone::WHETSTONE_ELEMENTAL_MATRIX_FAILED) {
      Errors::Message msg("Matrix_MFD: unexpected failure of LAPACK in WhetStone.");
      Exceptions::amanzi_throw(msg);
    }

    if (ok == WhetStone::WHETSTONE_ELEMENTAL_MATRIX_OK) nokay_++;
    if (ok == WhetStone::WHETSTONE_ELEMENTAL_MATRIX_PASSED) npassed_++;
  }

  // sum up the numbers across processors
  int nokay_tmp = nokay_, npassed_tmp = npassed_;
  mesh_->get_comm()->SumAll(&nokay_tmp, &nokay_, 1);
  mesh_->get_comm()->SumAll(&npassed_tmp, &npassed_, 1);
}


/* ******************************************************************
 * Calculate elemental stiffness matrices.
 ****************************************************************** */
void MatrixMFD::CreateMFDstiffnessMatrices(
    const Teuchos::Ptr<const CompositeVector>& Krel) {

  int dim = mesh_->space_dimension();
  WhetStone::MFD3D mfd(mesh_);
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  Aff_cells_.clear();
  Afc_cells_.clear();
  Acf_cells_.clear();
  Acc_cells_.clear();

  Teuchos::RCP<const Epetra_MultiVector> Krel_cell;
  Teuchos::RCP<const Epetra_MultiVector> Krel_face;
  if (Krel != Teuchos::null) {
    if (Krel->has_component("cell")) {
      Krel_cell = Krel->ViewComponent("cell",false);
    }
    if (Krel->has_component("face")) {
      Krel_face = Krel->ViewComponent("face",true);
      *Krel_ = *(*Krel_face)(0);
    }
  }

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  for (int c=0; c!=ncells; ++c) {
	mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
	int nfaces = faces.size();

	Teuchos::SerialDenseMatrix<int, double>& Mff = Mff_cells_[c];
	Teuchos::SerialDenseMatrix<int, double> Bff(nfaces,nfaces);
	Epetra_SerialDenseVector Bcf(nfaces), Bfc(nfaces);

	for (int n=0; n!=nfaces; ++n) {
	  for (int m=0; m!=nfaces; ++m) {
		Bff(m, n) = Mff(m,n) * ( Krel_cell == Teuchos::null ? 1. : (*Krel_cell)[0][c] );
	  }
	}

	double matsum = 0.0;
	for (int n=0; n!=nfaces; ++n) {
	  double rowsum = 0.0, colsum = 0.0;
	  for (int m=0; m!=nfaces; ++m) {
		if (Krel_face == Teuchos::null) {
		  colsum += Bff(m, n);
		} else {
		  colsum += Bff(m,n) * (*Krel_face)[0][faces[m]];
		}
		rowsum += Bff(n, m);
	  }

	  Bcf(n) = -colsum;
	  Bfc(n) = -rowsum;
	  matsum += colsum;
	}

    Aff_cells_.push_back(Bff);
    Afc_cells_.push_back(Bfc);
    Acf_cells_.push_back(Bcf);
    Acc_cells_.push_back(matsum);
  }
}


/* ******************************************************************
 * Simply allocates memory.
 ****************************************************************** */
void MatrixMFD::CreateMFDrhsVectors() {
  Ff_cells_.clear();
  Fc_cells_.clear();

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  for (int c=0; c!=ncells; ++c) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    Epetra_SerialDenseVector Ff(nfaces);  // Entries are initilaized to 0.0.
    double Fc = 0.0;

    Ff_cells_.push_back(Ff);
    Fc_cells_.push_back(Fc);
  }
}

/* ******************************************************************
 *  Create work vectors for Apply-ing the operator from/to Epetra_Vectors
 * (Supervectors) instead of CompositeVectors -- for use with AztecOO.
 * This should likely only be called with a non-ghosted sample vector.
 ****************************************************************** */
// void MatrixMFD::InitializeSuperVecs(const CompositeVector& sample) {
//   vector_x_ = Teuchos::rcp(new CompositeVector(sample));
//   vector_y_ = Teuchos::rcp(new CompositeVector(sample));
//   supermap_ = vector_x_->supermap();
// }

/* ******************************************************************
 * Applies boundary conditions to elemental stiffness matrices and
 * creates elemental rigth-hand-sides.
 ****************************************************************** */
void MatrixMFD::ApplyBoundaryConditions(const std::vector<Matrix_bc>& bc_markers,
        const std::vector<double>& bc_values) {
  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  int nfaces = mesh_->num_entities(AmanziMesh::FACE, AmanziMesh::OWNED);
  AmanziMesh::Entity_ID_List faces;
  AmanziMesh::Entity_ID_List cells;
  std::vector<int> dirs;

  for (int c=0; c!=ncells; ++c) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    Teuchos::SerialDenseMatrix<int, double>& Bff = Aff_cells_[c];  // B means elemental.
    Epetra_SerialDenseVector& Bfc = Afc_cells_[c];
    Epetra_SerialDenseVector& Bcf = Acf_cells_[c];

    Epetra_SerialDenseVector& Ff = Ff_cells_[c];
    double& Fc = Fc_cells_[c];

    for (int n=0; n!=nfaces; ++n) {
      int f=faces[n];
      if (bc_markers[f] == MATRIX_BC_DIRICHLET) {
        for (int m=0; m!=nfaces; ++m) {
          Ff[m] -= Bff(m, n) * bc_values[f];
          Bff(n, m) = Bff(m, n) = 0.0;
        }
        Fc -= Bcf(n) * bc_values[f];
        Bcf(n) = Bfc(n) = 0.0;

        Bff(n, n) = 1.0;
        Ff[n] = bc_values[f];
      } else if (bc_markers[f] == MATRIX_BC_FLUX) {
        if (std::abs(bc_values[f]) > 0.) {
          Ff[n] -= bc_values[f] * mesh_->face_area(f) / (*Krel_)[f];
        }
      }
    }
  }
}


/* ******************************************************************
 * Initialize Trilinos matrices. It must be called only once.
 * If matrix is non-symmetric, we generate transpose of the matrix
 * block Afc_ to reuse cf_graph; otherwise, pointer Afc_ = Acf_.
 ****************************************************************** */
void MatrixMFD::SymbolicAssembleGlobalMatrices() {
  const Epetra_Map& cmap = mesh_->cell_map(false);
  const Epetra_Map& fmap = mesh_->face_map(false);
  const Epetra_Map& fmap_wghost = mesh_->face_map(true);

  int avg_entries_row = (mesh_->space_dimension() == 2) ? MFD_QUAD_FACES : MFD_HEX_FACES;

  // allocate the graphs
  Teuchos::RCP<Epetra_CrsGraph> cf_graph =
      Teuchos::rcp(new Epetra_CrsGraph(Copy, cmap, fmap_wghost, avg_entries_row, false));
  Teuchos::RCP<Epetra_FECrsGraph> ff_graph =
      Teuchos::rcp(new Epetra_FECrsGraph(Copy, fmap, 2*avg_entries_row));

  // fill the graphs
  FillMatrixGraphs_(cf_graph.ptr(), ff_graph.ptr());

  int ierr = cf_graph->FillComplete(fmap, cmap);
  ASSERT(!ierr);
  ierr = ff_graph->GlobalAssemble();  // Symbolic graph is complete.
  ASSERT(!ierr);

  // allocate the matrices
  CreateMatrices_(*cf_graph, *ff_graph);
}


void MatrixMFD::FillMatrixGraphs_(const Teuchos::Ptr<Epetra_CrsGraph> cf_graph,
          const Teuchos::Ptr<Epetra_FECrsGraph> ff_graph) {
  const Epetra_Map& cmap = mesh_->cell_map(false);
  const Epetra_Map& fmap = mesh_->face_map(false);
  const Epetra_Map& fmap_wghost = mesh_->face_map(true);

  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;
  int faces_LID[MFD_MAX_FACES];  // Contigious memory is required.
  int faces_GID[MFD_MAX_FACES];

  // fill the graphs
  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  for (int c=0; c!=ncells; ++c) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    for (int n=0; n!=nfaces; ++n) {
      faces_LID[n] = faces[n];
      faces_GID[n] = fmap_wghost.GID(faces_LID[n]);
    }
    cf_graph->InsertMyIndices(c, nfaces, faces_LID);
    ff_graph->InsertGlobalIndices(nfaces, faces_GID, nfaces, faces_GID);
  }
}

void MatrixMFD::CreateMatrices_(const Epetra_CrsGraph& cf_graph,
        const Epetra_FECrsGraph& ff_graph) {
  // create global matrices
  const Epetra_Map& cmap = mesh_->cell_map(false);
  Acc_ = Teuchos::rcp(new Epetra_Vector(cmap));

  Acf_ = Teuchos::rcp(new Epetra_CrsMatrix(Copy, cf_graph));
  Aff_ = Teuchos::rcp(new Epetra_FECrsMatrix(Copy, ff_graph));
  Sff_ = Teuchos::rcp(new Epetra_FECrsMatrix(Copy, ff_graph));
  Aff_->GlobalAssemble();
  Sff_->GlobalAssemble();

  if (flag_symmetry_) {
    Afc_ = Acf_;
  } else {
    Afc_ = Teuchos::rcp(new Epetra_CrsMatrix(Copy, cf_graph));
  }

  // create the RHS
  std::vector<std::string> names(2);
  names[0] = "cell";
  names[1] = "face";

  std::vector<AmanziMesh::Entity_kind> locations(2);
  locations[0] = AmanziMesh::CELL;
  locations[1] = AmanziMesh::FACE;

  std::vector<int> num_dofs(2,1);
  rhs_ = Teuchos::rcp(new CompositeVector(mesh_, names, locations, num_dofs, true));
  rhs_->CreateData();

  // Krel
  const Epetra_Map& fmap = mesh_->face_map(false);
  Krel_ = Teuchos::rcp(new Epetra_Vector(fmap));
  Krel_->PutScalar(1.);
}


/* ******************************************************************
 * Convert elemental mass matrices into stiffness matrices and
 * assemble them into four global matrices.
 * We need an auxiliary GHOST-based vector to assemble the RHS.
 ****************************************************************** */
void MatrixMFD::AssembleGlobalMatrices() {
  Aff_->PutScalar(0.0);

  const Epetra_Map& fmap_wghost = mesh_->face_map(true);
  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;
  int faces_LID[MFD_MAX_FACES];
  int faces_GID[MFD_MAX_FACES];

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  for (int c=0; c!=ncells; ++c) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    for (int n=0; n!=nfaces; ++n) {
      faces_LID[n] = faces[n];
      faces_GID[n] = fmap_wghost.GID(faces_LID[n]);
    }
    (*Acc_)[c] = Acc_cells_[c];
    (*Acf_).ReplaceMyValues(c, nfaces, Acf_cells_[c].Values(), faces_LID);
    (*Aff_).SumIntoGlobalValues(nfaces, faces_GID, Aff_cells_[c].values());

    if (!flag_symmetry_)
      (*Afc_).ReplaceMyValues(c, nfaces, Afc_cells_[c].Values(), faces_LID);
  }
  (*Aff_).GlobalAssemble();

  // We repeat some of the loops for code clarity.
  rhs_->ViewComponent("face", true)->PutScalar(0.0);
  for (int c=0; c!=ncells; ++c) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    (*rhs_)("cell",c) = Fc_cells_[c];

    for (int n=0; n!=nfaces; ++n) {
      int f = faces[n];
      (*rhs_)("face",f) += Ff_cells_[c][n];
    }
  }
  rhs_->GatherGhostedToMaster("face");

  //   exit(0);

}


/* ******************************************************************
 * Compute the face Schur complement of 2x2 block matrix.
 ****************************************************************** */
void MatrixMFD::ComputeSchurComplement(const std::vector<Matrix_bc>& bc_markers,
        const std::vector<double>& bc_values) {
  Sff_->PutScalar(0.0);

  AmanziMesh::Entity_ID_List faces_LID;
  std::vector<int> dirs;
  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);

  for (int c=0; c!=ncells; ++c) {
    mesh_->cell_get_faces_and_dirs(c, &faces_LID, &dirs);
    int nfaces = faces_LID.size();
    Epetra_SerialDenseMatrix Schur(nfaces, nfaces);

    Epetra_SerialDenseVector& Bcf = Acf_cells_[c];
    Epetra_SerialDenseVector& Bfc = Afc_cells_[c];

    for (int n=0; n!=nfaces; ++n) {
      for (int m=0; m!=nfaces; ++m) {
        Schur(n, m) = Aff_cells_[c](n, m) - Bfc[n] * Bcf[m] / (*Acc_)[c];
      }
    }

    for (int n=0; n!=nfaces; ++n) {  // Symbolic boundary conditions
      int f=faces_LID[n];
      if (bc_markers[f] == MATRIX_BC_DIRICHLET) {
        for (int m=0; m!=nfaces; ++m) Schur(n, m) = Schur(m, n) = 0.0;
        Schur(n, n) = 1.0;
      }
    }

    Epetra_IntSerialDenseVector faces_GID(nfaces);
    for (int n=0; n!=nfaces; ++n) faces_GID[n] = (*Acf_).ColMap().GID(faces_LID[n]);
    (*Sff_).SumIntoGlobalValues(faces_GID, Schur);
  }
  (*Sff_).GlobalAssemble();
}

/* ******************************************************************
 * Parallel matvec product A * X.
 ****************************************************************** */
// int MatrixMFD::Apply(const Epetra_MultiVector& X, Epetra_MultiVector& Y) const {
//   vector_x_->DataFromSuperVector(*X(0));
//   vector_y_->DataFromSuperVector(*Y(0));
//   Apply(*vector_x_, vector_y_);
// }

// int MatrixMFD::ApplyInverse(const Epetra_MultiVector& X, Epetra_MultiVector& Y) const {
//   vector_x_->DataFromSuperVector(*X(0));
//   vector_y_->DataFromSuperVector(*Y(0));
//   ApplyInverse(*vector_x_, vector_y_);
// }

/* ******************************************************************
 * Parallel matvec product A * X.
 ****************************************************************** */
void MatrixMFD::Apply(const CompositeVector& X,
                      const Teuchos::Ptr<CompositeVector>& Y) const {
  int ierr;

  // Face unknowns:  Yf = Aff_ * Xf + Afc_ * Xc
  ierr = (*Aff_).Multiply(false, *X.ViewComponent("face",false),
                          *Y->ViewComponent("face", false));

  Epetra_MultiVector Tf(*Y->ViewComponent("face", false));
  ierr |= (*Afc_).Multiply(true, *X.ViewComponent("cell",false), Tf);  // Afc_ is kept in transpose form
  Y->ViewComponent("face",false)->Update(1.0, Tf, 1.0);

  // Cell unknowns:  Yc = Acf_ * Xf + Acc_ * Xc
  ierr |= (*Acf_).Multiply(false, *X.ViewComponent("face", false),
                           *Y->ViewComponent("cell", false));  // It performs the required parallel communications.
  ierr |= Y->ViewComponent("cell", false)->Multiply(1.0, *Acc_, *X.ViewComponent("cell", false), 1.0);

  if (ierr) {
    Errors::Message msg("MatrixMFD::Apply has failed to calculate Y = inv(A) * X.");
    Exceptions::amanzi_throw(msg);
  }
}


/* ******************************************************************
 * The OWNED cell-based and face-based d.o.f. are packed together into
 * the X and Y Epetra vectors, with the cell-based in the first part.
 *
 * WARNING: When invoked by AztecOO the arguments X and Y may be
 * aliased: possibly the same object or different views of the same
 * underlying data. Thus, we do not assign to Y until the end.
 *
 * NOTE however that this is broken for AztecOO since we use CompositeVectors,
 * not Epetra_MultiVectors.
 *
 ****************************************************************** */
void MatrixMFD::ApplyInverse(const CompositeVector& X,
                             const Teuchos::Ptr<CompositeVector>& Y) const {
  if (prec_method_ == PREC_METHOD_NULL) {
    Errors::Message msg("MatrixMFD::ApplyInverse requires a specified preconditioner method");
    Exceptions::amanzi_throw(msg);
  }

  // Temporary cell and face vectors.
  Epetra_MultiVector Tc(*Y->ViewComponent("cell", false));
  Epetra_MultiVector Tf(*Y->ViewComponent("face", false));

  // FORWARD ELIMINATION:  Tf = Xf - Afc_ inv(Acc_) Xc
  int ierr;
  ierr  = Tc.ReciprocalMultiply(1.0, *Acc_, *X.ViewComponent("cell", false), 0.0);
  ierr |= (*Afc_).Multiply(true, Tc, Tf);  // Afc_ is kept in transpose form
  Tf.Update(1.0, *X.ViewComponent("face", false), -1.0);

  // Solve the Schur complement system Sff_ * Yf = Tf.
  if (prec_method_ == TRILINOS_ML) {
    ierr |= ml_prec_->ApplyInverse(Tf, *Y->ViewComponent("face", false));
  } else if (prec_method_ == TRILINOS_ILU) {
    ierr |= ilu_prec_->ApplyInverse(Tf, *Y->ViewComponent("face", false));
  } else if (prec_method_ == TRILINOS_BLOCK_ILU) {
    ierr |= ifp_prec_->ApplyInverse(Tf, *Y->ViewComponent("face", false));
#ifdef HAVE_HYPRE
  } else if (prec_method_ == HYPRE_AMG || prec_method_ == HYPRE_EUCLID) {
    ierr != IfpHypre_Sff_->ApplyInverse(Tf, *Y->ViewComponent("face", false));
#endif
  } else {
    ASSERT(0);
  }

  // BACKWARD SUBSTITUTION:  Yc = inv(Acc_) (Xc - Acf_ Yf)
  ierr |= (*Acf_).Multiply(false, *Y->ViewComponent("face", false), Tc);  // It performs the required parallel communications.

  Tc.Update(1.0, *X.ViewComponent("cell", false), -1.0);

  ierr |= Y->ViewComponent("cell", false)->ReciprocalMultiply(1.0, *Acc_, Tc, 0.0);

  if (ierr) {
    Errors::Message msg("MatrixMFD::ApplyInverse has failed in calculating y = A*x.");
    Exceptions::amanzi_throw(msg);
  }
}


/* ******************************************************************
 * Linear algebra operations with matrices: r = f - A * x
 ****************************************************************** */
void MatrixMFD::ComputeResidual(const CompositeVector& solution,
        const Teuchos::Ptr<CompositeVector>& residual) const {
  Apply(solution, residual);
  residual->Update(1.0, *rhs_, -1.0);
}


/* ******************************************************************
 * Linear algebra operations with matrices: r = A * x - f
 ****************************************************************** */
void MatrixMFD::ComputeNegativeResidual(const CompositeVector& solution,
        const Teuchos::Ptr<CompositeVector>& residual) const {
  Apply(solution, residual);
  residual->Update(-1.0, *rhs_, 1.0);

//  std::cout << "  soln = " << (solution)("cell",0) << ", " << (solution)("face",0) << std::endl;
//  std::cout << "  rhs = " << (*rhs_)("cell",0) << ", " << (*rhs_)("face",0) << std::endl;
//  std::cout << "  res = " << (*residual)("cell",0) << ", " << (*residual)("face",0) << std::endl;
}


/* ******************************************************************
 * Initialization of the preconditioner
 ****************************************************************** */
void MatrixMFD::InitPreconditioner() {
  if (prec_method_ == TRILINOS_ML) {
    ml_plist_ =  plist_.sublist("ML Parameters");
    ml_prec_ = Teuchos::rcp(new ML_Epetra::MultiLevelPreconditioner(*Sff_, ml_plist_, false));
  } else if (prec_method_ == TRILINOS_ILU) {
    ilu_plist_ = plist_.sublist("ILU Parameters");
  } else if (prec_method_ == TRILINOS_BLOCK_ILU) {
    ifp_plist_ = plist_.sublist("Block ILU Parameters");
#ifdef HAVE_HYPRE
  } else if (prec_method_ == HYPRE_AMG) {
    // read some boomer amg parameters
    hypre_plist_ = plist_.sublist("HYPRE AMG Parameters");
    hypre_ncycles_ = hypre_plist_.get<int>("number of cycles",5);
    hypre_nsmooth_ = hypre_plist_.get<int>("number of smoothing iterations",3);
    hypre_tol_ = hypre_plist_.get<double>("tolerance",0.0);
    hypre_strong_threshold_ = hypre_plist_.get<double>("strong threshold",0.25);
  } else if (prec_method_ == HYPRE_EUCLID) {
    hypre_plist_ = plist_.sublist("HYPRE Euclid Parameters");
  } else if (prec_method_ == HYPRE_PARASAILS) {
    hypre_plist_ = plist_.sublist("HYPRE ParaSails Parameters");
#endif
  }
}


/* ******************************************************************
 * Rebuild preconditioner.
 ****************************************************************** */
void MatrixMFD::UpdatePreconditioner() {
  if (prec_method_ == TRILINOS_ML) {
    if (ml_prec_->IsPreconditionerComputed()) ml_prec_->DestroyPreconditioner();
    ml_prec_->SetParameterList(ml_plist_);
    ml_prec_->ComputePreconditioner();
  } else if (prec_method_ == TRILINOS_ILU) {
    ilu_prec_ = Teuchos::rcp(new Ifpack_ILU(&*Sff_));
    ilu_prec_->SetParameters(ilu_plist_);
    ilu_prec_->Initialize();
    ilu_prec_->Compute();
  } else if (prec_method_ == TRILINOS_BLOCK_ILU) {
    Ifpack factory;
    std::string prectype("ILU");
    int ovl = ifp_plist_.get<int>("overlap",0);
    ifp_plist_.set<std::string>("schwarz: combine mode","Add");
    ifp_prec_ = Teuchos::rcp(factory.Create(prectype, &*Sff_, ovl));
    ifp_prec_->SetParameters(ifp_plist_);
    ifp_prec_->Initialize();
    ifp_prec_->Compute();
#ifdef HAVE_HYPRE
  } else if (prec_method_ == HYPRE_AMG) {
    IfpHypre_Sff_ = Teuchos::rcp(new Ifpack_Hypre(&*Sff_));
    Teuchos::RCP<FunctionParameter> functs[8];
    functs[0] = Teuchos::rcp(new FunctionParameter(Preconditioner, &HYPRE_BoomerAMGSetCoarsenType, 0));
    functs[1] = Teuchos::rcp(new FunctionParameter(Preconditioner, &HYPRE_BoomerAMGSetPrintLevel, 0));
    functs[2] = Teuchos::rcp(new FunctionParameter(Preconditioner, &HYPRE_BoomerAMGSetNumSweeps, hypre_nsmooth_));
    functs[3] = Teuchos::rcp(new FunctionParameter(Preconditioner, &HYPRE_BoomerAMGSetMaxIter, hypre_ncycles_));
    functs[4] = Teuchos::rcp(new FunctionParameter(Preconditioner, &HYPRE_BoomerAMGSetRelaxType, 6));
    functs[5] = Teuchos::rcp(new FunctionParameter(Preconditioner, &HYPRE_BoomerAMGSetStrongThreshold, hypre_strong_threshold_));
    functs[6] = Teuchos::rcp(new FunctionParameter(Preconditioner, &HYPRE_BoomerAMGSetTol, hypre_tol_));
    functs[7] = Teuchos::rcp(new FunctionParameter(Preconditioner, &HYPRE_BoomerAMGSetCycleType, 1));

    Teuchos::ParameterList hypre_list;
    hypre_list.set("Preconditioner", BoomerAMG);
    hypre_list.set("SolveOrPrecondition", Preconditioner);
    hypre_list.set("SetPreconditioner", true);
    hypre_list.set("NumFunctions", 8);
    hypre_list.set<Teuchos::RCP<FunctionParameter>*>("Functions", functs);

    IfpHypre_Sff_->SetParameters(hypre_list);
    IfpHypre_Sff_->Initialize();
    IfpHypre_Sff_->Compute();
  } else if (prec_method_ == HYPRE_EUCLID) {
    IfpHypre_Sff_ = Teuchos::rcp(new Ifpack_Hypre(&*Sff_));

    Teuchos::ParameterList hypre_list;
    hypre_list.set("Preconditioner", Euclid);
    hypre_list.set("SolveOrPrecondition", Preconditioner);
    hypre_list.set("SetPreconditioner", true);
    hypre_list.set("NumFunctions", 0);

    IfpHypre_Sff_->SetParameters(hypre_list);
    IfpHypre_Sff_->Initialize();
    IfpHypre_Sff_->Compute();
  } else if (prec_method_ == HYPRE_PARASAILS) {
    IfpHypre_Sff_ = Teuchos::rcp(new Ifpack_Hypre(&*Sff_));

    Teuchos::ParameterList hypre_list;
    hypre_list.set("Preconditioner", ParaSails);
    hypre_list.set("SolveOrPrecondition", Preconditioner);
    hypre_list.set("SetPreconditioner", true);
    hypre_list.set("NumFunctions", 0);

    IfpHypre_Sff_->SetParameters(hypre_list);
    IfpHypre_Sff_->Initialize();
    IfpHypre_Sff_->Compute();
#endif
  }
}



/* ******************************************************************
 * WARNING: Routines requires original mass matrices (Aff_cells_), i.e.
 * before boundary conditions were imposed.
 *
 * WARNING: Since diffusive flux is not continuous, we derive it only
 * once (using flag) and in exactly the same manner as in routine
 * Flow_PK::addGravityFluxes_DarcyFlux.
 *
 * WARNING: THIS ASSUMES solution has previously be communicated to update
 * ghost faces.
 ****************************************************************** */
void MatrixMFD::DeriveFlux(const CompositeVector& solution,
                           const Teuchos::Ptr<CompositeVector>& flux) const {

  AmanziMesh::Entity_ID_List faces;
  std::vector<double> dp;
  std::vector<int> dirs;

  flux->PutScalar(0.);

  int ncells = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  int nfaces_owned = flux->size("face",false);

  std::vector<bool> done(nfaces_owned, false);
  const Epetra_MultiVector& soln_cells = *solution.ViewComponent("cell",false);
  const Epetra_MultiVector& soln_faces = *solution.ViewComponent("face",true);
  Epetra_MultiVector& flux_v = *flux->ViewComponent("face",false);

  for (int c=0; c!=ncells; ++c) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    dp.resize(nfaces);
    for (int n=0; n!=nfaces; ++n) {
      int f = faces[n];
      dp[n] = soln_cells[0][c] - soln_faces[0][f];
    }

    for (int n=0; n!=nfaces; ++n) {
      int f = faces[n];
      if (f < nfaces_owned && !done[f]) {
        double s = 0.0;
        for (int m=0; m!=nfaces; ++m) {
          s += Aff_cells_[c](n, m) * dp[m];
        }

        flux_v[0][f] = s * dirs[n] * (*Krel_)[f];
        done[f] = true;
      }
    }
  }

  // ensure post-condition - we got them all
  for (int f=0; f!=nfaces_owned; ++f) {
    ASSERT(done[f]);
  }

}


/* ******************************************************************
 * Derive Darcy velocity in cells.
 * WARNING: It cannot be consistent with the Darcy flux.
 ****************************************************************** */
void MatrixMFD::DeriveCellVelocity(const CompositeVector& flux,
        const Teuchos::Ptr<CompositeVector>& velocity) const {

  Teuchos::LAPACK<int, double> lapack;

  int dim = mesh_->space_dimension();
  Teuchos::SerialDenseMatrix<int, double> matrix(dim, dim);
  double rhs_cell[dim];

  AmanziMesh::Entity_ID_List faces;
  std::vector<int> dirs;

  int ncells_owned = mesh_->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  for (int c=0; c!=ncells_owned; ++c) {
    mesh_->cell_get_faces_and_dirs(c, &faces, &dirs);
    int nfaces = faces.size();

    for (int i=0; i!=dim; ++i) rhs_cell[i] = 0.0;
    matrix.putScalar(0.0);

    for (int n=0; n!=nfaces; ++n) {  // populate least-square matrix
      int f = faces[n];
      const AmanziGeometry::Point& normal = mesh_->face_normal(f);
      double area = mesh_->face_area(f);

      for (int i=0; i!=dim; ++i) {
        rhs_cell[i] += normal[i] * flux("face",0,f);
        matrix(i,i) += normal[i] * normal[i];
        for (int j=i+1; j!=dim; ++j) {
          matrix(j,i) = matrix(i,j) += normal[i] * normal[j];
        }
      }
    }

    int info;
    lapack.POSV('U', dim, 1, matrix.values(), dim, rhs_cell, dim, &info);

    for (int i=0; i!=dim; ++i) (*velocity)("cell",i,c) = rhs_cell[i];
  }
}


// development methods
/* ******************************************************************
 * Reduce the pressure-lambda-system to lambda-system via ellimination
 * of the known pressure. Structure of the global system is preserved
 * but off-diagola blocks are zeroed-out.
 ****************************************************************** */
void MatrixMFD::UpdateConsistentFaceConstraints(const Teuchos::Ptr<CompositeVector>& u) {
  Teuchos::RCP<Epetra_MultiVector> uc = u->ViewComponent("cell", false);
  Teuchos::RCP<Epetra_MultiVector> rhs_f = rhs_->ViewComponent("face", false);

  Teuchos::RCP<Epetra_MultiVector> update_f = Teuchos::rcp(new Epetra_MultiVector(*rhs_f));
  Afc_->Multiply(true, *uc, *update_f);  // Afc is kept in the transpose form.
  rhs_f->Update(-1.0, *update_f, 1.0);

  // Replace the schur complement so it can be used as a face-only system
  *Sff_ = *Aff_;

  // Update the preconditioner with a solver
  UpdatePreconditioner();

  // Use this entry to get appropriate faces.
  int ierr;
  if (prec_method_ == TRILINOS_ML) {
    ierr = ml_prec_->ApplyInverse(*rhs_f, *u->ViewComponent("face",false));
  } else if (prec_method_ == TRILINOS_ILU) {
    ierr = ilu_prec_->ApplyInverse(*rhs_f, *u->ViewComponent("face",false));
  } else if (prec_method_ == TRILINOS_BLOCK_ILU) {
    ierr = ifp_prec_->ApplyInverse(*rhs_f, *u->ViewComponent("face",false));
#ifdef HAVE_HYPRE
  } else if (prec_method_ == HYPRE_AMG || prec_method_ == HYPRE_EUCLID) {
    ierr = IfpHypre_Sff_->ApplyInverse(*rhs_f, *u->ViewComponent("face",false));
#endif
  } else {
    ASSERT(0);
  }
}


void MatrixMFD::UpdateConsistentFaceCorrection(const CompositeVector& u,
        const Teuchos::Ptr<CompositeVector>& Pu) {
  Teuchos::RCP<const Epetra_MultiVector> Pu_c = Pu->ViewComponent("cell", false);
  Epetra_MultiVector& Pu_f = *Pu->ViewComponent("face", false);
  const Epetra_MultiVector& u_f = *u.ViewComponent("face", false);
  Epetra_MultiVector update_f(u_f);
  Afc_->Multiply(true, *Pu_c, update_f);  // Afc is kept in the transpose form.
  update_f.Update(1., u_f, -1.);

  // Replace the schur complement so it can be used as a face-only system
  *Sff_ = *Aff_;

  // Update the preconditioner with a solver
  UpdatePreconditioner();

  // Use this entry to get appropriate faces.
  int ierr;
  if (prec_method_ == TRILINOS_ML) {
    ierr = ml_prec_->ApplyInverse(update_f, Pu_f);
  } else if (prec_method_ == TRILINOS_ILU) {
    ierr = ilu_prec_->ApplyInverse(update_f, Pu_f);
  } else if (prec_method_ == TRILINOS_BLOCK_ILU) {
    ierr = ifp_prec_->ApplyInverse(update_f, Pu_f);
#ifdef HAVE_HYPRE
  } else if (prec_method_ == HYPRE_AMG || prec_method_ == HYPRE_EUCLID) {
    ierr = IfpHypre_Sff_->ApplyInverse(update_f, Pu_f);
#endif
  }
}


}  // namespace AmanziFlow
}  // namespace Amanzi
