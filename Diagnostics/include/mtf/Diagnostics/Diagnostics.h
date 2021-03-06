#ifndef MTF_DIAGNOSTICS_H
#define MTF_DIAGNOSTICS_H

#include "mtf/AM/AppearanceModel.h"
#include "mtf/SSM/StateSpaceModel.h"
#include <memory>

_MTF_BEGIN_NAMESPACE

struct DiagnosticsParams{

	enum class UpdateType { Additive, Compositional };

	UpdateType update_type;
	bool show_data;
	bool show_corners;
	bool show_patches;
	bool enable_validation;
	double validation_prec;

	DiagnosticsParams(UpdateType _update_type,
		bool _show_data, bool _show_corners,
		bool _show_patches, bool _enable_validation,
		double _validation_prec);
	DiagnosticsParams(const DiagnosticsParams *params = nullptr);
};

class Diagnostics{

public:
	enum class AnalyticalDataType {
		Norm, FeatNorm, Likelihood,
		// Jacobians
		StdJac, ESMJac, DiffOfJacs,
		// Hessians
		Std, ESM, InitSelf, CurrSelf,
		Std2, ESM2, InitSelf2, CurrSelf2,
		SumOfStd, SumOfStd2, SumOfSelf, SumOfSelf2
	};
	enum class NumericalDataType {
		Jacobian, Hessian, NHessian
	};

	typedef DiagnosticsParams ParamType;
	typedef AMDist DistType;
	typedef std::shared_ptr<mtf::StateSpaceModel> SSM;
	typedef std::shared_ptr<mtf::AppearanceModel> AM;
	typedef std::shared_ptr<const DistType> DistTypePtr;
	typedef AnalyticalDataType ADT;
	typedef NumericalDataType NDT;
	typedef ParamType::UpdateType UpdateType;

	unsigned int frame_id;
	unsigned int n_pix;

	unsigned int am_dist_size;
	unsigned int ssm_state_size, am_state_size;
	unsigned int state_size;

	Diagnostics(AM _am, SSM _ssm,
		const ParamType *diag_params = nullptr);
	virtual ~Diagnostics();
	int inputType() const { return am->inputType(); }
	void setImage(const cv::Mat &img){ am->setCurrImg(img); }

	void initialize(const cv::Mat &corners, const bool *gen_flags);
	void update(const cv::Mat &corners);

	void generateAnalyticalData(VectorXd &param_range,
		int n_pts, ADT data_type, const char* fname = nullptr);
	void generateAnalyticalData3D(VectorXd &x_vec, VectorXd &y_vec,
		const VectorXd &param_range, int n_pts,
		const Vector2i &state_ids, ADT data_type,
		const char* fname = nullptr);
	void generateInverseAnalyticalData(VectorXd &param_range,
		int n_pts, ADT data_type, const char* fname = nullptr);

	void generateNumericalData(VectorXd &param_range_vec, int n_pts,
		NDT data_type, const char* fname, double grad_diff);
	void generateInverseNumericalData(VectorXd &param_range_vec,
		int n_pts, NDT data_type, const char* fname, double grad_diff);

	void generateSSMParamData(VectorXd &param_range_vec,
		int n_pts, const char* fname);
	double getADTVal(ADT data_type, int state_id);
	double getInvADTVal(ADT data_type, int state_id);

	const char* toString(ADT data_type);
	const char* toString(NDT data_type);
	const MatrixXd& getData(){ return diagnostics_data; }

	AM& getAM(){ return am; }
	SSM& getSSM(){ return ssm; }

private:
	ParamType params;

	AM am;
	SSM ssm;
	DistTypePtr dist_func;

	VectorXd inv_ssm_state, inv_am_state;

	//! N x S jacobians of the pix values w.r.t the SSM state vector where N = resx * resy
	//! is the no. of pixels in the object patch
	MatrixXd init_pix_jacobian, init_pix_hessian;
	MatrixXd curr_pix_jacobian, curr_pix_hessian;
	MatrixXd mean_pix_jacobian, mean_pix_hessian;

	//! 1 x S Jacobian of the AM error norm w.r.t. SSM state vector
	RowVectorXd similarity_jacobian;
	//! S x S Hessian of the AM error norm w.r.t. SSM state vector
	MatrixXd hessian, init_hessian;
	MatrixXd init_self_hessian, init_self_hessian2;
	MatrixXd curr_self_hessian, curr_self_hessian2;

	VectorXd init_dist_vec, curr_dist_vec;

	Matrix3d warp_update;
	char *update_name;
	MatrixXd diagnostics_data;

	char *init_patch_win_name;
	char *curr_patch_win_name;

	char *init_img_win_name;
	char *curr_img_win_name;

	cv::Mat init_patch, init_patch_uchar;
	cv::Mat curr_patch, curr_patch_uchar;

	cv::Mat init_img_cv, init_img_cv_uchar;
	cv::Mat curr_img_cv, curr_img_cv_uchar;

	EigImgT init_img;

	cv::Mat init_corners;
	cv::Point2d curr_corners_cv[4];

	MatrixXd ssm_grad_norm;
	RowVectorXd ssm_grad_norm_mean;

	void updateSSM(const VectorXd &state_update);
	void resetSSM(const VectorXd &state_update);

	void updateAM(const VectorXd &state_update);
	void resetAM(const VectorXd &state_update);

	void updateState(const VectorXd &state_update, unsigned int state_id);
	void resetState(const VectorXd &state_update, unsigned int state_id);

	void initializePixJacobian();
	void updateInitPixJacobian();
	void updateCurrPixJacobian();

	void initializePixHessian();
	void updateInitPixHessian();
	void updateCurrPixHessian();

	void updateInitSimilarity();
	void updateInitGrad();
	void updateInitHess();
	void updateInitSelfHess();

	void updateCurrSimilarity();
	void updateCurrGrad();
	void updateCurrSelfHess();

	bool validateHessians(const MatrixXd &self_hessian);

	void drawCurrCorners(cv::Mat &img, int state_id = -1, int thickness = 2,
		bool write_text = true, cv::Scalar corners_col = (0, 0, 255));
};
_MTF_END_NAMESPACE

#endif

