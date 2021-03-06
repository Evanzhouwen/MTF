#include "mtf/SM/NT/ESM.h"
#include "mtf/Utilities/miscUtils.h"
#include "mtf/Utilities/excpUtils.h"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"

_MTF_BEGIN_NAMESPACE

namespace nt{

	ESM::ESM(AM _am, SSM _ssm, const ParamType *esm_params) :
		SearchMethod(_am, _ssm), params(esm_params){
		printf("\n");
		printf("Using Efficient Second order Minimization (NT) SM with:\n");
		printf("max_iters: %d\n", params.max_iters);
		printf("epsilon: %f\n", params.epsilon);
		printf("jac_type: %d\n", params.jac_type);
		printf("hess_type: %d\n", params.hess_type);
		printf("sec_ord_hess: %d\n", params.sec_ord_hess);
		printf("chained_warp: %d\n", params.chained_warp);
		printf("leven_marq: %d\n", params.leven_marq);
		if(params.leven_marq){
			printf("lm_delta_init: %f\n", params.lm_delta_init);
			printf("lm_delta_update: %f\n", params.lm_delta_update);
		}
		printf("enable_learning: %d\n", params.enable_learning);
		printf("spi_type: %s\n", ParamType::toString(params.spi_type));
		printf("debug_mode: %d\n", params.debug_mode);

		printf("appearance model: %s\n", am->name.c_str());
		printf("state space model: %s\n", ssm->name.c_str());
		printf("\n");

		name = "esm_nt";
		log_fname = "log/mtf_esm_log.txt";
		time_fname = "log/mtf_esm_times.txt";

		frame_id = 0;

#ifndef DISABLE_SPI
		spi_enabled = params.spi_type != SPIType::None;
		if(spi_enabled){
			if(!ssm->supportsSPI())
				throw utils::FunctonNotImplemented("ESM::SSM does not support SPI");
			if(!am->supportsSPI())
				throw utils::FunctonNotImplemented("ESM::AM does not support SPI");
			pix_mask.resize(am->getPatchSize());
			pix_mask.fill(true);
			switch(params.spi_type){
			case SPIType::None:
				break;
			case SPIType::PixDiff:
				spi.reset(new utils::spi::PixDiff(pix_mask, params.spi_params));
				break;
			case SPIType::Gradient:
				spi.reset(new utils::spi::Gradient(pix_mask, params.spi_params));
				break;
			case SPIType::GFTT:
				spi.reset(new utils::spi::GFTT(pix_mask, params.spi_params));
				break;
			default:
				throw utils::InvalidArgument("ESM::updateSPIMask( :: Invalid SPI type provided");
			}
			pix_mask2.resize(am->getPatchSize());
			pix_mask_img = cv::Mat(am->getResY(), am->getResX(), CV_8UC1, pix_mask2.data());
			spi_win_name = "pix_mask_img";
		}
#endif
		printf("Using %s\n", ESMParams::toString(params.jac_type));
		const char *hess_order = params.sec_ord_hess ? "Second" : "First";
		printf("Using %s order %s\n", hess_order, ESMParams::toString(params.hess_type));
		if(params.leven_marq){
			printf("Using Levenberg Marquardt formulation...\n");
		}
		ssm_state_size = ssm->getStateSize();
		am_state_size = am->getStateSize();
		state_size = ssm_state_size + am_state_size;
		printf("ssm_state_size: %d am_state_size: %d state_size: %d\n",
			ssm_state_size, am_state_size, state_size);

		state_update.resize(state_size);
		ssm_update.resize(ssm_state_size);
		am_update.resize(am_state_size);
		inv_ssm_update.resize(ssm_state_size);
		inv_am_update.resize(am_state_size);

		jacobian.resize(state_size);
		hessian.resize(state_size, state_size);
		if(params.hess_type == HessType::SumOfSelf){
			init_self_hessian.resize(state_size, state_size);
		}

		init_pix_jacobian.resize(am->getPatchSize(), ssm_state_size);
		curr_pix_jacobian.resize(am->getPatchSize(), ssm_state_size);

		if(params.jac_type == JacType::Original || params.hess_type == HessType::Original){
			mean_pix_jacobian.resize(am->getPatchSize(), ssm_state_size);
		}
		if(params.sec_ord_hess){
			init_pix_hessian.resize(ssm_state_size*ssm_state_size, am->getPatchSize());
			if(params.hess_type != HessType::InitialSelf){
				curr_pix_hessian.resize(ssm_state_size*ssm_state_size, am->getPatchSize());
				if(params.hess_type == HessType::Original){
					mean_pix_hessian.resize(ssm_state_size*ssm_state_size, am->getPatchSize());
				}
			}
		}
	}

	void ESM::initialize(const cv::Mat &corners){
		start_timer();

		am->clearInitStatus();
		ssm->clearInitStatus();

		frame_id = 0;
		ssm->initialize(corners, am->getNChannels());
		am->initializePixVals(ssm->getPts());

		initializePixJacobian();

#ifndef DISABLE_SPI		
		if(spi_enabled){
			initializeSPIMask();
			am->setSPIMask(pix_mask.data());
			ssm->setSPIMask(pix_mask.data());
		}
#endif		
		if(params.sec_ord_hess){
			initializePixHessian();
		}
		am->initializeSimilarity();
		am->initializeGrad();
		am->initializeHess();

		if(params.hess_type == HessType::InitialSelf || params.hess_type == HessType::SumOfSelf){
			if(params.sec_ord_hess){
				am->cmptSelfHessian(hessian, init_pix_jacobian, init_pix_hessian);
			} else{
				am->cmptSelfHessian(hessian, init_pix_jacobian);
			}
			init_self_hessian = hessian;
		}
		ssm->getCorners(cv_corners_mat);

		end_timer();
		write_interval(time_fname, "w");
	}

	void ESM::setRegion(const cv::Mat& corners){
		ssm->setCorners(corners);
		// since the above command completely resets the SSM state including its initial points,
		// any quantities that depend on these, like init_pix_jacobian and init_pix_hessian,
		// must be recomputed along with quantities that depend on them in turn.
		ssm->cmptInitPixJacobian(init_pix_jacobian, am->getInitPixGrad());
		if(params.sec_ord_hess){
			ssm->cmptInitPixHessian(init_pix_hessian, am->getInitPixHess(), am->getInitPixGrad());
		}
		if(params.hess_type == HessType::InitialSelf || params.hess_type == HessType::SumOfSelf){
			if(params.sec_ord_hess){
				am->cmptSelfHessian(hessian, init_pix_jacobian, init_pix_hessian);
			} else{
				am->cmptSelfHessian(hessian, init_pix_jacobian);
			}
			init_self_hessian = hessian;
		}
		ssm->getCorners(cv_corners_mat);
	}

	void ESM::update(){
		++frame_id;
		write_frame_id(frame_id);

		double prev_similarity = 0;
		double leven_marq_delta = params.lm_delta_init;
		bool state_reset = false;

		am->setFirstIter();
		for(int iter_id = 0; iter_id < params.max_iters; ++iter_id){
			init_timer();

#ifndef DISABLE_SPI		
			if(spi_enabled){
				//! pixel values and gradients need to be extracted without SPI
				am->clearSPIMask();
				ssm->clearSPIMask();
			}
#endif	
			//! extract pixel values from the current image at the latest known position of the object
			am->updatePixVals(ssm->getPts());
			record_event("am->updatePixVals");

#ifndef DISABLE_SPI	
			//! don't want to compute the pixel Jacobian unnecessary if it is not needed for SPI 
			//! in case LM test decides that the previous update needs to be undone
			updatePixJacobian();
			if(spi_enabled){
				updateSPIMask();
				am->setSPIMask(pix_mask.data());
				ssm->setSPIMask(pix_mask.data());
			}
#endif	
			//! compute the prerequisites for the gradient functions
			am->updateSimilarity(false);
			record_event("am->updateSimilarity");

			if(params.leven_marq && !state_reset){
				double curr_similarity = am->getSimilarity();
				if(iter_id > 0){
					if(curr_similarity < prev_similarity){
						leven_marq_delta *= params.lm_delta_update;
						//! undo the last update
						ssm->invertState(inv_ssm_update, ssm_update);
						ssm->compositionalUpdate(inv_ssm_update);
						am->invertState(inv_am_update, am_update);
						am->updateState(inv_am_update);

						if(params.debug_mode){
							printf("curr_similarity: %20.14f\t prev_similarity: %20.14f\n",
								curr_similarity, prev_similarity);
							utils::printScalar(leven_marq_delta, "leven_marq_delta");
						}
						state_reset = true;
						continue;
					}
					if(curr_similarity > prev_similarity){
						leven_marq_delta /= params.lm_delta_update;
					}
				}
				prev_similarity = curr_similarity;
			}
			state_reset = false;
#ifdef DISABLE_SPI	
			//! if pixel gradient is not needed for SPI, it is computed only if the LM test 
			//! has not rejected the previous update to avoid any unnecessary computations
			updatePixJacobian();
#endif
			if(params.jac_type == JacType::Original || params.hess_type == HessType::Original){
				mean_pix_jacobian = (init_pix_jacobian + curr_pix_jacobian) / 2.0;
				record_event("mean_pix_jacobian");
			}
			if(params.sec_ord_hess && params.hess_type != HessType::InitialSelf){
				updatePixHessian();
			}

			// update the gradient of the error norm w.r.t. current pixel values
			am->updateCurrGrad();
			record_event("am->updateCurrGrad");

			// update the gradient of the error norm w.r.t. initial pixel values
			am->updateInitGrad();
			record_event("am->updateInitGrad");

			cmptJacobian();
			cmptHessian();

			if(params.leven_marq){
				//utils::printMatrix(hessian, "original hessian");
				MatrixXd diag_hessian = hessian.diagonal().asDiagonal();
				//utils::printMatrix(diag_hessian, "diag_hessian");
				hessian += leven_marq_delta*diag_hessian;
				//utils::printMatrix(hessian, "LM hessian");
			}

			//utils::printMatrix(hessian, "hessian");
			//utils::printMatrix(jacobian, "jacobian");
			state_update = -hessian.colPivHouseholderQr().solve(jacobian.transpose());
			record_event("state_update");

			ssm_update = state_update.head(ssm_state_size);
			am_update = state_update.tail(am_state_size);

			prev_corners = ssm->getCorners();
			updateState();

			//double update_norm = ssm_update.lpNorm<1>();
			double update_norm = (prev_corners - ssm->getCorners()).squaredNorm();
			record_event("update_norm");

			write_data(time_fname);

			if(update_norm < params.epsilon){
				if(params.debug_mode){
					printf("n_iters: %d\n", iter_id + 1);
				}
				break;
			}
#ifndef DISABLE_SPI		
			if(spi_enabled){ showSPIMask(); }
#endif
			am->clearFirstIter();
		}
		if(params.enable_learning){
			am->updateModel(ssm->getPts());
		}
		ssm->getCorners(cv_corners_mat);
	}

	void ESM::cmptJacobian(){
		switch(params.jac_type){
		case JacType::Original:
			// take the mean at the level of the jacobian of the pixel values wrt SSM parameters, 
			//then use this mean jacobian to compute the Jacobian of the error norm wrt SSM parameters
			am->cmptCurrJacobian(jacobian, mean_pix_jacobian);
			record_event("am->cmptCurrJacobian");
			break;
		case JacType::DiffOfJacs:
			// compute the mean difference between the Jacobians of the error norm w.r.t. initial AND current values of SSM parameters
			am->cmptDifferenceOfJacobians(jacobian, init_pix_jacobian, curr_pix_jacobian);
			jacobian *= 0.5;
			record_event("am->cmptDifferenceOfJacobians");
			break;
		}
	}

	void ESM::cmptHessian(){
		switch(params.hess_type){
		case HessType::InitialSelf:
			if(params.leven_marq){
				hessian = init_self_hessian;
			}
			break;
		case HessType::Original:
			if(params.sec_ord_hess){
				mean_pix_hessian = (init_pix_hessian + curr_pix_hessian) / 2.0;
				record_event("mean_pix_hessian");
				am->cmptCurrHessian(hessian, mean_pix_jacobian, mean_pix_hessian);
				record_event("am->cmptCurrHessian (second order)");
			} else{
				am->cmptCurrHessian(hessian, mean_pix_jacobian);
				record_event("am->cmptCurrHessian (first order)");
			}
			break;
		case HessType::SumOfStd:
			if(params.sec_ord_hess){
				am->cmptSumOfHessians(hessian, init_pix_jacobian, curr_pix_jacobian,
					init_pix_hessian, curr_pix_hessian);
				record_event("am->cmptSumOfHessians (second order)");
			} else{
				am->cmptSumOfHessians(hessian, init_pix_jacobian, curr_pix_jacobian);
				record_event("am->cmptSumOfHessians (first order)");
			}
			hessian *= 0.5;
			break;
		case HessType::SumOfSelf:
			if(params.sec_ord_hess){
				am->cmptSelfHessian(hessian, curr_pix_jacobian, curr_pix_hessian);
				record_event("am->cmptSelfHessian (second order)");
			} else{
				am->cmptSelfHessian(hessian, curr_pix_jacobian);
				record_event("am->cmptSelfHessian (first order)");
			}
			hessian = (hessian + init_self_hessian) * 0.5;
			break;
		case HessType::CurrentSelf:
			if(params.sec_ord_hess){
				am->cmptSelfHessian(hessian, curr_pix_jacobian, curr_pix_hessian);
				record_event("am->cmptSelfHessian (second order)");
			} else{
				am->cmptSelfHessian(hessian, curr_pix_jacobian);
				record_event("am->cmptSelfHessian (first order)");
			}
			break;
		case HessType::Std:
			if(params.sec_ord_hess){
				am->cmptCurrHessian(hessian, curr_pix_jacobian, curr_pix_hessian);
				record_event("am->cmptCurrHessian (second order)");
			} else{
				am->cmptCurrHessian(hessian, curr_pix_jacobian);
				record_event("am->cmptCurrHessian (first order)");
			}
			break;
		}
	}

	void ESM::initializePixJacobian(){
		if(params.chained_warp){
			am->initializePixGrad(ssm->getPts());
			ssm->cmptWarpedPixJacobian(init_pix_jacobian, am->getInitPixGrad());
		} else{
			ssm->initializeGradPts(am->getGradOffset());
			am->initializePixGrad(ssm->getGradPts());
			ssm->cmptInitPixJacobian(init_pix_jacobian, am->getInitPixGrad());
		}
	}

	void ESM::updatePixJacobian(){
		//! compute the Jacobian of pixel values w.r.t. SSM parameters
		if(params.chained_warp){
			am->updatePixGrad(ssm->getPts());
			record_event("am->updatePixGrad");
			ssm->cmptWarpedPixJacobian(curr_pix_jacobian, am->getCurrPixGrad());
			record_event("ssm->cmptWarpedPixJacobian");
		} else{
			ssm->updateGradPts(am->getGradOffset());
			record_event("ssm->updateGradPts");
			// compute pixel gradient of the current image warped with the current warp
			am->updatePixGrad(ssm->getGradPts());
			record_event("am->updatePixGrad");
			// multiply the pixel gradient with the SSM Jacobian to get the Jacobian of pixel values w.r.t. SSM parameters
			ssm->cmptInitPixJacobian(curr_pix_jacobian, am->getCurrPixGrad());
			record_event("ssm->cmptInitPixJacobian");
		}
	}

	void ESM::initializePixHessian(){
		if(params.chained_warp){
			am->initializePixHess(ssm->getPts());
			ssm->cmptWarpedPixHessian(init_pix_hessian, am->getInitPixHess(),
				am->getInitPixGrad());
		} else{
			ssm->initializeHessPts(am->getHessOffset());
			am->initializePixHess(ssm->getPts(), ssm->getHessPts());
			ssm->cmptInitPixHessian(init_pix_hessian, am->getInitPixHess(), am->getInitPixGrad());
		}
	}

	void ESM::updatePixHessian(){
		if(params.chained_warp){
			am->updatePixHess(ssm->getPts());
			record_event("am->updatePixHess");
			ssm->cmptWarpedPixHessian(curr_pix_hessian, am->getCurrPixHess(), am->getCurrPixGrad());
			record_event("ssm->cmptWarpedPixHessian");
		} else{
			ssm->updateHessPts(am->getHessOffset());
			record_event("ssm->updateHessPts");
			am->updatePixHess(ssm->getPts(), ssm->getHessPts());
			record_event("am->updatePixHess");
			ssm->cmptInitPixHessian(curr_pix_hessian, am->getCurrPixHess(), am->getCurrPixGrad());
			record_event("ssm->cmptInitPixHessian");
		}
	}

	void ESM::updateState(){
		ssm->compositionalUpdate(ssm_update);
		record_event("ssm->compositionalUpdate");
		am->updateState(am_update);
		record_event("am->updateState");
	}
#ifndef DISABLE_SPI		
	// support for Selective Pixel Integration	
	void ESM::initializeSPIMask(){
		cv::namedWindow(spi_win_name);
		switch(params.spi_type){
		case SPIType::None:
			break;
		case SPIType::PixDiff:
			static_cast<utils::spi::PixDiff*>(spi.get())->initialize(am->getInitPixVals());
			break;
		case SPIType::Gradient:
			static_cast<utils::spi::Gradient*>(spi.get())->initialize(am->getInitPixGrad());
			break;
		case SPIType::GFTT:
			static_cast<utils::spi::GFTT*>(spi.get())->initialize(am->getInitPixVals(), am->getResX(), am->getResY());
			break;
		default:
			throw utils::InvalidArgument("ESM::initializeSPIMask :: Invalid SPI type provided");
		}
	}

	void ESM::updateSPIMask(){
		switch(params.spi_type){
		case SPIType::None:
			break;
		case SPIType::PixDiff:
			static_cast<utils::spi::PixDiff*>(spi.get())->update(am->getInitPixVals(), am->getCurrPixVals());
			break;
		case SPIType::Gradient:
			static_cast<utils::spi::Gradient*>(spi.get())->update(am->getCurrPixGrad());
			break;
		case SPIType::GFTT:
			static_cast<utils::spi::GFTT*>(spi.get())->update(am->getCurrPixVals());
			break;
		default:
			throw utils::InvalidArgument("ESM::updateSPIMask :: Invalid SPI type provided");
		}
		if(params.debug_mode){
			int active_pixels = pix_mask.count();
			utils::printScalar(active_pixels, "active_pixels", "%d");
		}
	}
	void ESM::showSPIMask(){
		pix_mask2.noalias() = pix_mask.cast<unsigned char>() * 255;
		//pix_mask_img = cv::Mat(am->getResY(), am->getResX(), am->getNChannels() == 3 ? CV_64FC3 : CV_64FC1,
		//	const_cast<double*>(am->getCurrPixVals().data())).mul(
		//	cv::Mat(am->getResY(), am->getResX(), CV_8UC1, pix_mask.data()));
		cv::Mat pix_mask_img_resized(pix_mask_img.rows * 3, pix_mask_img.cols * 3, CV_8UC1);
		cv::resize(pix_mask_img, pix_mask_img_resized, pix_mask_img_resized.size());
		imshow(spi_win_name, pix_mask_img_resized);
	}
#endif	

}
_MTF_END_NAMESPACE
