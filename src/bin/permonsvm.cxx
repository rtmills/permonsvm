#include <algorithm>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string/regex.hpp>

#include <mpi.h>
#include <petscksp.h>
#include <fllopqps.h>

extern char Fllop_input_dir[FLLOP_MAX_PATH_LEN], Fllop_output_dir[FLLOP_MAX_PATH_LEN];

static MPI_Comm     comm;
static PetscMPIInt  rank, commsize;
static PetscLogStage loadStage, prepStage, solvStage, postStage, saveStage;

template<typename T=int>
bool ParseSourceFileLine(std::string &line, std::vector<T> &cols, T &yi) {
    using namespace std;
    using namespace boost;

    regex r("\\s+|:");

    vector<string> parsed_values;
    int col;

    algorithm::split_regex(parsed_values, line, r);

    yi = boost::lexical_cast<T>(parsed_values[0]);

    cols.clear();
    for (int i = 1; i < parsed_values.size(); i += 2) {
        try {
            col = boost::lexical_cast<T>(parsed_values[i]);
        } catch (bad_lexical_cast &e) {
            cout << "Caught bad lexical cast with error " << e.what() << endl;
            return (false);
        } catch (...) {
            cout << "Unknown exception caught!" << endl;
            return (false);
        }
        cols.push_back(col);
    }

    return (true);
}

template<typename T=int>
bool ParseSourceFileLine(std::string &line, std::vector<T> &cols) {
    using namespace std;
    using namespace boost;

    regex r("\\s+|:");

    vector<string> parsed_values;
    int col;

    algorithm::split_regex(parsed_values, line, r);

    cols.clear();
    for (int i = 1; i < parsed_values.size(); i += 2) {
        try {
            col = boost::lexical_cast<T>(parsed_values[i]);
        } catch (bad_lexical_cast &e) {
            cout << "Caught bad lexical cast with error " << e.what() << endl;
            return (false);
        } catch (...) {
            cout << "Unknown exception caught!" << endl;
            return (false);
        }
        cols.push_back(col);
    }

    return (true);
}

template<typename T=int>
bool GetMatrixStructure(const std::string &filename, std::vector<T> &nnz_per_row, T &nnz_max, T &num_cols) {
    using namespace std;

    ifstream fl_hldr;
    string tmp_line; vector<T> tmp_vec;
    T tmp_cols, nnz;

    fl_hldr.open(filename.c_str());

    num_cols = 0;
    nnz_max = 0;
    if (fl_hldr.good()) {
        while (getline(fl_hldr, tmp_line)) {
            ParseSourceFileLine(tmp_line, tmp_vec);
            tmp_cols = tmp_vec.back();
            if (tmp_cols > num_cols) {
                num_cols = tmp_cols;
            }
            nnz = tmp_vec.size();
            if (nnz > nnz_max) {
              nnz_max = nnz;
            }
            nnz_per_row.push_back(nnz);
        }
    } else {
        return (false);
    }

    return (true);
}

#undef __FUNCT__
#define __FUNCT__ "PermonExcapeSetValues"
PetscErrorCode PermonExcapeSetValues(Mat Xt, Vec y, PetscInt nnz_max, const std::string &filename) {
    using namespace std;

    ifstream fl_hldr;
    string tmp_line; vector<PetscInt> idxn;
    PetscInt yi, n_cols;
    PetscScalar ones[nnz_max];

    PetscFunctionBeginUser;
    TRY( MatGetSize(Xt, NULL, &n_cols) );
    fl_hldr.open(filename.c_str());
    TRY( fl_hldr.fail() );

    std::fill_n(ones, nnz_max, 1.0);

    PetscInt i = 0;
    while (getline(fl_hldr, tmp_line)) {
      ParseSourceFileLine(tmp_line, idxn, yi);
      std::transform(idxn.begin(), idxn.end(), idxn.begin(), [](PetscInt a) { return a-1; });  // decrement 1-based values of idxn to be 0-based
      TRY( MatSetValues(Xt, 1, &i, idxn.size(), &idxn[0], ones, INSERT_VALUES) );
      TRY( VecSetValue(y,i,yi,INSERT_VALUES) );
      i++;
    }

    TRY( MatAssemblyBegin(Xt, MAT_FINAL_ASSEMBLY) );
    TRY( MatAssemblyEnd(Xt, MAT_FINAL_ASSEMBLY) );
    TRY( VecAssemblyBegin(y) );
    TRY( VecAssemblyEnd(y) );
    PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PermonExcapeLoadData"
PetscErrorCode PermonExcapeLoadData(MPI_Comm comm, const char *data_file_name, Mat *Xt_new, Vec *y_new)
{
  using namespace std;
  vector<PetscInt> nnz_per_row;
  PetscInt n_examples, n_attributes, nnz_max;
  Mat Xt;
  Vec y;

  PetscFunctionBeginI;
  TRY( !GetMatrixStructure(string(data_file_name), nnz_per_row, nnz_max, n_attributes) );
  n_examples = nnz_per_row.size();

  TRY( MatCreate(comm, &Xt) );
  TRY( MatSetSizes(Xt, n_examples, n_attributes, PETSC_DECIDE, PETSC_DECIDE) );
  TRY( MatSetOptionsPrefix(Xt, "Xt_") );
  TRY( MatSetFromOptions(Xt) );
  TRY( MatSeqAIJSetPreallocation(Xt, -1, &nnz_per_row[0]) );
  //TODO MatMPIAIJSetPreallocation

  TRY( VecCreate(comm,&y) );
  TRY( VecSetSizes(y, n_examples, PETSC_DECIDE) );
  TRY( VecSetOptionsPrefix(y, "y_") );
  TRY( VecSetFromOptions(y) );

  TRY( PermonExcapeSetValues(Xt, y, nnz_max, data_file_name) );

  *Xt_new = Xt;
  *y_new = y;
  PetscFunctionReturnI(0);
}

#undef __FUNCT__
#define __FUNCT__ "PermonExcapeDataToQP"
PetscErrorCode PermonExcapeDataToQP(Mat Xt, Vec y, QP *qp_new)
{
  MPI_Comm comm;
  QP qp;
  Mat X,XtX;
  Vec e;
  Mat BEt;
  PetscInt n;

  PetscFunctionBeginI;
  TRY( PetscObjectGetComm((PetscObject)Xt, &comm) );
  TRY( QPCreate(comm,&qp) );

  TRY( FllopMatTranspose(Xt,MAT_TRANSPOSE_CHEAPEST,&X) );
  TRY( MatCreateNormal(X,&XtX) );
  TRY( MatDiagonalScale(XtX,y,y) );
  TRY( QPSetOperator(qp,XtX) );
  TRY( MatDestroy(&X) );
  TRY( MatDestroy(&XtX) );

  TRY( VecDuplicate(y,&e) );
  TRY( VecSet(e,1.0) );
  TRY( QPSetRhs(qp, e) );
  TRY( VecDestroy(&e) );

  //TODO BEt: new shell matrix for dot product y'*x

  //TODO get from user lambda or C = 1/(2*lambda*n)

  //TODO box constraints  0 <= c <= C

  *qp_new = qp;
  PetscFunctionReturnI(0);
}

#undef __FUNCT__
#define __FUNCT__ "testQPS_files"
PetscErrorCode testQPS_files()
{
  QP             qp  = NULL;
  QPS            qps = NULL;
  const QPSType  qpstype;
  Mat            Xt;
  Vec            y;

  PetscFunctionBeginI;
  /* ------------------------------------------------------------------------ */
  TRY( PetscLogStageRegister("load data", &loadStage) );
  TRY( PetscLogStagePush(loadStage) );
  {
    //TRY( PermonExcapeLoadData(comm,"gene_GSK3B_level6_1000_1_1_1_train.txt", &Xt, &y) );
    TRY( PermonExcapeLoadData(comm,"dummy.txt", &Xt, &y) );
    TRY( PermonExcapeDataToQP(Xt, y, &qp) );
    TRY( MatDestroy(&Xt) );
    TRY( VecDestroy(&y) );
  }
  TRY( PetscLogStagePop() );

  /* ------------------------------------------------------------------------ */  
  TRY( PetscLogStageRegister("preprocessing", &prepStage) );
  TRY( PetscLogStagePush(prepStage) );
  {
    //TRY( QPTFromOptions(qp) );

    TRY( QPSCreate(comm, &qps) );
    TRY( QPSSetQP(qps, qp) );
    TRY( QPSSetAutoPostSolve(qps, PETSC_FALSE) );
    TRY( QPSSetFromOptions(qps) );

    TRY( QPSSetUp(qps) );
    TRY( QPSGetType(qps, &qpstype) );
    TRY( PetscInfo1(fllop, "Using automatically chosen QPS type %s\n", qpstype) );
  }
  TRY( PetscLogStagePop() );

  /* ------------------------------------------------------------------------ */  
  TRY( PetscLogStageRegister("solve", &solvStage) );
  TRY( PetscLogStagePush(solvStage) );
  {
    TRY( QPSSolve(qps) );
  }
  TRY( PetscLogStagePop() );

  /* ------------------------------------------------------------------------ */
  TRY( PetscLogStageRegister("postsolve", &postStage) );
  TRY( PetscLogStagePush(postStage) );
  {
    TRY( QPSPostSolve(qps) );
  }
  TRY( PetscLogStagePop() );

  /* ------------------------------------------------------------------------ */  
  TRY( PetscLogStageRegister("save data", &saveStage) );
  TRY( PetscLogStagePush(saveStage) );
  //
  TRY( PetscLogStagePop() );

  TRY( QPSDestroy(&qps) );
  TRY( QPDestroy(&qp) );
  PetscFunctionReturnI(0);
}

#undef __FUNCT__
#define __FUNCT__ "main"
int main(int argc, char** argv)
{
  FllopInitialize(&argc, &argv, (char*) 0);

  PetscFunctionBegin;
  comm = PETSC_COMM_WORLD;
  TRY( MPI_Comm_rank(comm, &rank) );
  TRY( MPI_Comm_size(comm, &commsize) );

  FLLOP_ASSERT(commsize==1,"currently only sequential");

  TRY( FllopSetTrace(PETSC_TRUE) );
  TRY( FllopSetDebug(PETSC_TRUE) );
  TRY( FllopSetObjectInfo(PETSC_TRUE) );

  TRY( PetscPrintf(comm, "PETSC_DIR:\t" PETSC_DIR "\n") );
  TRY( PetscPrintf(comm, "PETSC_ARCH:\t" PETSC_ARCH "\n") );
#ifdef PETSC_RELEASE_DATE
#define DATE PETSC_RELEASE_DATE
#else
#define DATE PETSC_VERSION_DATE
#endif
  TRY( PetscPrintf(comm, "PETSc version:\t%d.%d.%d patch %d (%s)\n", PETSC_VERSION_MAJOR, PETSC_VERSION_MINOR, PETSC_VERSION_SUBMINOR, PETSC_VERSION_PATCH, DATE) );
#undef DATE
  TRY( PetscPrintf(comm,"Input dir:\t%s\n", Fllop_input_dir) );
  TRY( PetscPrintf(comm,"Output dir:\t%s\n", Fllop_output_dir) );

  TRY( testQPS_files() );
  TRY( FllopFinalize() );
  PetscFunctionReturn(0);
}
