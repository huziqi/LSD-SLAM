/**
* This file is part of LSD-SLAM.
*
* Copyright 2013 Jakob Engel <engelj at in dot tum dot de> (Technical University of Munich)
* For more information see <http://vision.in.tum.de/lsdslam> 
*
* LSD-SLAM is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* LSD-SLAM is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with LSD-SLAM. If not, see <http://www.gnu.org/licenses/>.
*/

#include "SE3Tracker.h"
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/opencv.hpp>
#include "DataStructures/Frame.h"
#include "Tracking/TrackingReference.h"
#include "util/globalFuncs.h"
#include "IOWrapper/ImageDisplay.h"
#include "Tracking/least_squares.h"

#include <Eigen/Core>

#include <cmath>

using namespace cv;
using namespace std;

namespace lsd_slam
{


#if defined(ENABLE_NEON)
	#define callOptimized(function, arguments) function##NEON arguments
#else
	#if defined(ENABLE_SSE)
		#define callOptimized(function, arguments) (USESSE ? function##SSE arguments : function arguments)
	#else
		#define callOptimized(function, arguments) function arguments
	#endif
#endif


SE3Tracker::SE3Tracker(int w, int h, Eigen::Matrix3f K)
{
	width = w;
	height = h;

	this->K = K;
	fx = K(0,0);
	fy = K(1,1);
	cx = K(0,2);
	cy = K(1,2);

	settings = DenseDepthTrackerSettings();
	//settings.maxItsPerLvl[0] = 2;

	KInv = K.inverse();
	fxi = KInv(0,0);
	fyi = KInv(1,1);
	cxi = KInv(0,2);
	cyi = KInv(1,2);


	buf_warped_residual = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	buf_warped_dx = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	buf_warped_dy = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	buf_warped_x = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	buf_warped_y = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	buf_warped_z = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));

	buf_d = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	buf_idepthVar = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	buf_weight_p = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));

	//#
	project_buf_warped_residual_x = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	project_buf_warped_residual_y = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	project_buf_warped_dx = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	project_buf_warped_dy = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	project_buf_warped_x = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	project_buf_warped_y = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	project_buf_warped_z = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));

	project_buf_d = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	project_buf_idepthVar = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));
	project_buf_weight_p = (float*)Eigen::internal::aligned_malloc(w*h*sizeof(float));

	buf_warped_size = 0;
	project_buf_warped_size = 0;

	buf_fx = 0;
	buf_fy = 0;

	minidepthVar = 10; //初始化最小逆深度方差
	for(int level=0;level<PYRAMID_LEVELS;level++)
	{
		//初始化特征点匹配个数
		nmatches[level] = 0;
	}

	debugImageWeights = cv::Mat(height,width,CV_8UC3);
	debugImageResiduals = cv::Mat(height,width,CV_8UC3);
	debugImageSecondFrame = cv::Mat(height,width,CV_8UC3);
	debugImageOldImageWarped = cv::Mat(height,width,CV_8UC3);
	debugImageOldImageSource = cv::Mat(height,width,CV_8UC3);



	lastResidual = 0;
	iterationNumber = 0;
	pointUsage = 0;
	lastGoodCount = lastBadCount = 0;

	diverged = false;
}

SE3Tracker::~SE3Tracker()
{
	debugImageResiduals.release();
	debugImageWeights.release();
	debugImageSecondFrame.release();
	debugImageOldImageSource.release();
	debugImageOldImageWarped.release();


	Eigen::internal::aligned_free((void*)buf_warped_residual);
	Eigen::internal::aligned_free((void*)buf_warped_dx);
	Eigen::internal::aligned_free((void*)buf_warped_dy);
	Eigen::internal::aligned_free((void*)buf_warped_x);
	Eigen::internal::aligned_free((void*)buf_warped_y);
	Eigen::internal::aligned_free((void*)buf_warped_z);

	Eigen::internal::aligned_free((void*)buf_d);
	Eigen::internal::aligned_free((void*)buf_idepthVar);
	Eigen::internal::aligned_free((void*)buf_weight_p);

	//#
	Eigen::internal::aligned_free((void*)project_buf_warped_residual_x);
	Eigen::internal::aligned_free((void*)project_buf_warped_residual_y);
	Eigen::internal::aligned_free((void*)project_buf_warped_dx);
	Eigen::internal::aligned_free((void*)project_buf_warped_dy);
	Eigen::internal::aligned_free((void*)project_buf_warped_x);
	Eigen::internal::aligned_free((void*)project_buf_warped_y);
	Eigen::internal::aligned_free((void*)project_buf_warped_z);

	Eigen::internal::aligned_free((void*)project_buf_d);
	Eigen::internal::aligned_free((void*)project_buf_idepthVar);
	Eigen::internal::aligned_free((void*)project_buf_weight_p);
}



// tracks a frame.
// first_frame has depth, second_frame DOES NOT have depth.
float SE3Tracker::checkPermaRefOverlap(
		Frame* reference,
		SE3 referenceToFrameOrg)
{
	Sophus::SE3f referenceToFrame = referenceToFrameOrg.cast<float>();
	boost::unique_lock<boost::mutex> lock2 = boost::unique_lock<boost::mutex>(reference->permaRef_mutex);

	int w2 = reference->width(QUICK_KF_CHECK_LVL)-1;
	int h2 = reference->height(QUICK_KF_CHECK_LVL)-1;
	Eigen::Matrix3f KLvl = reference->K(QUICK_KF_CHECK_LVL);
	float fx_l = KLvl(0,0);
	float fy_l = KLvl(1,1);
	float cx_l = KLvl(0,2);
	float cy_l = KLvl(1,2);

	Eigen::Matrix3f rotMat = referenceToFrame.rotationMatrix();
	Eigen::Vector3f transVec = referenceToFrame.translation();

	const Eigen::Vector3f* refPoint_max = reference->permaRef_posData + reference->permaRefNumPts;
	const Eigen::Vector3f* refPoint = reference->permaRef_posData;

	float usageCount = 0;
	for(;refPoint<refPoint_max; refPoint++)
	{
		Eigen::Vector3f Wxp = rotMat * (*refPoint) + transVec;
		float u_new = (Wxp[0]/Wxp[2])*fx_l + cx_l;
		float v_new = (Wxp[1]/Wxp[2])*fy_l + cy_l;
		if((u_new > 0 && v_new > 0 && u_new < w2 && v_new < h2))
		{
			float depthChange = (*refPoint)[2] / Wxp[2];
			usageCount += depthChange < 1 ? depthChange : 1;
		}
	}

	pointUsage = usageCount / (float)reference->permaRefNumPts;
	return pointUsage;
}


// tracks a frame.
// first_frame has depth, second_frame DOES NOT have depth.
SE3 SE3Tracker::trackFrameOnPermaref(
		Frame* reference,
		Frame* frame,
		SE3 referenceToFrameOrg)
{

	Sophus::SE3f referenceToFrame = referenceToFrameOrg.cast<float>();

	boost::shared_lock<boost::shared_mutex> lock = frame->getActiveLock();
	boost::unique_lock<boost::mutex> lock2 = boost::unique_lock<boost::mutex>(reference->permaRef_mutex);

	affineEstimation_a = 1; affineEstimation_b = 0;

	NormalEquationsLeastSquares ls;
	diverged = false;
	trackingWasGood = true;

	callOptimized(calcResidualAndBuffers, (reference->permaRef_posData, reference->permaRef_colorAndVarData, 0, reference->permaRefNumPts, frame, referenceToFrame, QUICK_KF_CHECK_LVL, false));
	if(buf_warped_size < MIN_GOODPERALL_PIXEL_ABSMIN * (width>>QUICK_KF_CHECK_LVL)*(height>>QUICK_KF_CHECK_LVL))
	{
		diverged = true;
		trackingWasGood = false;
		return SE3();
	}
	if(useAffineLightningEstimation)
	{
		affineEstimation_a = affineEstimation_a_lastIt;
		affineEstimation_b = affineEstimation_b_lastIt;
	}
	float lastErr = callOptimized(calcWeightsAndResidual,(referenceToFrame));

	float LM_lambda = settings.lambdaInitialTestTrack;

	for(int iteration=0; iteration < settings.maxItsTestTrack; iteration++)
	{
		callOptimized(calculateWarpUpdate,(ls));


		int incTry=0;
		while(true)
		{
			// solve LS system with current lambda
			Vector6 b = -ls.b;
			Matrix6x6 A = ls.A;
			for(int i=0;i<6;i++) A(i,i) *= 1+LM_lambda;
			Vector6 inc = A.ldlt().solve(b);
			incTry++;

			// apply increment. pretty sure this way round is correct, but hard to test.
			Sophus::SE3f new_referenceToFrame = Sophus::SE3f::exp((inc)) * referenceToFrame;

			// re-evaluate residual
			callOptimized(calcResidualAndBuffers, (reference->permaRef_posData, reference->permaRef_colorAndVarData, 0, reference->permaRefNumPts, frame, new_referenceToFrame, QUICK_KF_CHECK_LVL, false));
			if(buf_warped_size < MIN_GOODPERALL_PIXEL_ABSMIN * (width>>QUICK_KF_CHECK_LVL)*(height>>QUICK_KF_CHECK_LVL))
			{
				diverged = true;
				trackingWasGood = false;
				return SE3();
			}
			float error = callOptimized(calcWeightsAndResidual,(new_referenceToFrame));


			// accept inc?
			if(error < lastErr)
			{
				// accept inc
				referenceToFrame = new_referenceToFrame;
				if(useAffineLightningEstimation)
				{
					affineEstimation_a = affineEstimation_a_lastIt;
					affineEstimation_b = affineEstimation_b_lastIt;
				}
				// converged?
				if(error / lastErr > settings.convergenceEpsTestTrack)
					iteration = settings.maxItsTestTrack;


				lastErr = error;


				if(LM_lambda <= 0.2)
					LM_lambda = 0;
				else
					LM_lambda *= settings.lambdaSuccessFac;

				break;
			}
			else
			{
				if(!(inc.dot(inc) > settings.stepSizeMinTestTrack))
				{
					iteration = settings.maxItsTestTrack;
					break;
				}

				if(LM_lambda == 0)
					LM_lambda = 0.2;
				else
					LM_lambda *= std::pow(settings.lambdaFailFac, incTry);
			}
		}
	}

	lastResidual = lastErr;

	trackingWasGood = !diverged
			&& lastGoodCount / (frame->width(QUICK_KF_CHECK_LVL)*frame->height(QUICK_KF_CHECK_LVL)) > MIN_GOODPERALL_PIXEL
			&& lastGoodCount / (lastGoodCount + lastBadCount) > MIN_GOODPERGOODBAD_PIXEL;

	return toSophus(referenceToFrame);
}





// tracks a frame.
// first_frame has depth, second_frame DOES NOT have depth.
SE3 SE3Tracker::trackFrame(
		TrackingReference* reference,
		Frame* frame,
		const SE3& frameToReference_initialEstimate)
{

	boost::shared_lock<boost::shared_mutex> lock = frame->getActiveLock();
	diverged = false;
	trackingWasGood = true;
	affineEstimation_a = 1; affineEstimation_b = 0;

	if(saveAllTrackingStages)
	{
		saveAllTrackingStages = false;
		saveAllTrackingStagesInternal = true;
	}
	
	if (plotTrackingIterationInfo)
	{
		const float* frameImage = frame->image();
		for (int row = 0; row < height; ++ row)
			for (int col = 0; col < width; ++ col)
				setPixelInCvMat(&debugImageSecondFrame,getGrayCvPixel(frameImage[col+row*width]), col, row, 1);
	}
	


	// ============ track frame ============
	Sophus::SE3f referenceToFrame = frameToReference_initialEstimate.inverse().cast<float>();
	NormalEquationsLeastSquares ls;

	

	int numCalcResidualCalls[PYRAMID_LEVELS];
	int numCalcWarpUpdateCalls[PYRAMID_LEVELS];

	float last_residual = 0;

	//============check idepth===============
	reference->makePointCloud(0);
	float sum_temp = 0;
	Eigen::Vector2f* VarDataPT = reference->colorAndVarData[0];
	
	for (int id = 0; id < reference->numData[0];id++)
	{
		sum_temp += (*VarDataPT)[1];
	}
	
	if(sum_temp/reference->numData[0]<0.02)
	{
		//==============联合逐层优化==============
		for(int lvl=SE3TRACKING_MAX_LEVEL-1;lvl >= SE3TRACKING_MIN_LEVEL;lvl--)//从第五层优化到第二层
		{
			numCalcResidualCalls[lvl] = 0;
			numCalcWarpUpdateCalls[lvl] = 0;

			ProjectionResiduals(reference, frame, referenceToFrame, lvl);
			reference->makePointCloud(lvl);

			//计算残差
			callOptimized(calcResidualAndBuffers, (reference->posData[lvl], reference->colorAndVarData[lvl], SE3TRACKING_MIN_LEVEL == lvl ? reference->pointPosInXYGrid[lvl] : 0, reference->numData[lvl], frame, referenceToFrame, lvl, (plotTracking && lvl == SE3TRACKING_MIN_LEVEL)));
			if(buf_warped_size < MIN_GOODPERALL_PIXEL_ABSMIN * (width>>lvl)*(height>>lvl))
			{
				diverged = true;
				trackingWasGood = false;
				return SE3();
			}

			if(useAffineLightningEstimation)//光照仿射变换
			{
				affineEstimation_a = affineEstimation_a_lastIt;
				affineEstimation_b = affineEstimation_b_lastIt;
			}
			float lastErr = callOptimized(calcWeightsAndResidual,(referenceToFrame)) + calcProjectWeights();//联合误差

			numCalcResidualCalls[lvl]++;


			float LM_lambda = settings.lambdaInitial[lvl];

			for(int iteration=0; iteration < settings.maxItsPerLvl[lvl]; iteration++)
			{

				//callOptimized(calculateWarpUpdate,(ls));////计算雅可比以及最小二乘系数
				calculateUnionWarpUpdate(ls); ////计算雅可比以及最小二乘系数

				numCalcWarpUpdateCalls[lvl]++;

				iterationNumber = iteration;

				int incTry=0;
				while(true)
				{
					// solve LS system with current lambda
					Vector6 b = -ls.b - ls.pro_b;
					Matrix6x6 A = ls.A + ls.pro_A;
					for (int i = 0; i < 6; i++)
						A(i, i) *= 1 + LM_lambda;
					Vector6 inc = A.ldlt().solve(b);
					incTry++;

					// apply increment. pretty sure this way round is correct, but hard to test.
					Sophus::SE3f new_referenceToFrame = Sophus::SE3f::exp((inc)) * referenceToFrame;
					//Sophus::SE3f new_referenceToFrame = referenceToFrame * Sophus::SE3f::exp((inc));


					// re-evaluate residual
					ProjectionResiduals(reference, frame, new_referenceToFrame, lvl);
					callOptimized(calcResidualAndBuffers, (reference->posData[lvl], reference->colorAndVarData[lvl], SE3TRACKING_MIN_LEVEL == lvl ? reference->pointPosInXYGrid[lvl] : 0, reference->numData[lvl], frame, new_referenceToFrame, lvl, (plotTracking && lvl == SE3TRACKING_MIN_LEVEL)));
					if(buf_warped_size < MIN_GOODPERALL_PIXEL_ABSMIN* (width>>lvl)*(height>>lvl))
					{
						diverged = true;
						trackingWasGood = false;
						return SE3();
					}

					float error = callOptimized(calcWeightsAndResidual,(new_referenceToFrame)) + calcProjectWeights();
					numCalcResidualCalls[lvl]++;


					// accept inc?
					if(error < lastErr)
					{
						// accept inc
						referenceToFrame = new_referenceToFrame;
						if(useAffineLightningEstimation)
						{
							affineEstimation_a = affineEstimation_a_lastIt;
							affineEstimation_b = affineEstimation_b_lastIt;
						}


						if(enablePrintDebugInfo && printTrackingIterationInfo)
						{
							// debug output
							printf("(%d-%d): ACCEPTED increment of %f with lambda %.1f, residual: %f -> %f\n",
									lvl,iteration, sqrt(inc.dot(inc)), LM_lambda, lastErr, error);

							printf("         p=%.4f %.4f %.4f %.4f %.4f %.4f\n",
									referenceToFrame.log()[0],referenceToFrame.log()[1],referenceToFrame.log()[2],
									referenceToFrame.log()[3],referenceToFrame.log()[4],referenceToFrame.log()[5]);
						}

						// converged?
						if(error / lastErr > settings.convergenceEps[lvl])
						{
							if(enablePrintDebugInfo && printTrackingIterationInfo)
							{
								printf("(%d-%d): FINISHED pyramid level (last residual reduction too small).\n",
										lvl,iteration);
							}
							iteration = settings.maxItsPerLvl[lvl];
						}

						last_residual = lastErr = error;


						if(LM_lambda <= 0.2)
							LM_lambda = 0;
						else
							LM_lambda *= settings.lambdaSuccessFac;

						break;
					}
					else
					{
						if(true)
						{
							printf("(%d-%d): REJECTED increment of %f with lambda %.1f, (residual: %f -> %f)\n",
									lvl,iteration, sqrt(inc.dot(inc)), LM_lambda, lastErr, error);
						}

						if(!(inc.dot(inc) > settings.stepSizeMin[lvl]))
						{
							if(true)
							{
								printf("(%d-%d): FINISHED pyramid level (stepsize too small).\n",
										lvl,iteration);
							}
							iteration = settings.maxItsPerLvl[lvl];
							break;
						}

						if(LM_lambda == 0)
							LM_lambda = 0.2;
						else
							LM_lambda *= std::pow(settings.lambdaFailFac, incTry);
					}
				}
			}
		}
	}
	else
	{
		//==============直接法逐层优化=============
		for(int lvl=SE3TRACKING_MAX_LEVEL-1;lvl >= SE3TRACKING_MIN_LEVEL;lvl--)//从第五层优化到第二层
		{
			numCalcResidualCalls[lvl] = 0;
			numCalcWarpUpdateCalls[lvl] = 0;

			reference->makePointCloud(lvl);

			//计算残差
			callOptimized(calcResidualAndBuffers, (reference->posData[lvl], reference->colorAndVarData[lvl], SE3TRACKING_MIN_LEVEL == lvl ? reference->pointPosInXYGrid[lvl] : 0, reference->numData[lvl], frame, referenceToFrame, lvl, (plotTracking && lvl == SE3TRACKING_MIN_LEVEL)));
			if(buf_warped_size < MIN_GOODPERALL_PIXEL_ABSMIN * (width>>lvl)*(height>>lvl))
			{
				diverged = true;
				trackingWasGood = false;
				return SE3();
			}

			if(useAffineLightningEstimation)//光照仿射变换
			{
				affineEstimation_a = affineEstimation_a_lastIt;
				affineEstimation_b = affineEstimation_b_lastIt;
			}
			float lastErr = callOptimized(calcWeightsAndResidual,(referenceToFrame));////计算归一化方差的光度误差系数，返回值是归一化光度误差均值

			numCalcResidualCalls[lvl]++;


			float LM_lambda = settings.lambdaInitial[lvl];

			for(int iteration=0; iteration < settings.maxItsPerLvl[lvl]; iteration++)
			{

				callOptimized(calculateWarpUpdate,(ls));////计算雅可比以及最小二乘系数

				numCalcWarpUpdateCalls[lvl]++;

				iterationNumber = iteration;

				int incTry=0;
				while(true)
				{
					// solve LS system with current lambda
					Vector6 b = -ls.b;
					Matrix6x6 A = ls.A;
					for(int i=0;i<6;i++) A(i,i) *= 1+LM_lambda;
					Vector6 inc = A.ldlt().solve(b);
					incTry++;

					// apply increment. pretty sure this way round is correct, but hard to test.
					Sophus::SE3f new_referenceToFrame = Sophus::SE3f::exp((inc)) * referenceToFrame;
					//Sophus::SE3f new_referenceToFrame = referenceToFrame * Sophus::SE3f::exp((inc));


					// re-evaluate residual
					callOptimized(calcResidualAndBuffers, (reference->posData[lvl], reference->colorAndVarData[lvl], SE3TRACKING_MIN_LEVEL == lvl ? reference->pointPosInXYGrid[lvl] : 0, reference->numData[lvl], frame, new_referenceToFrame, lvl, (plotTracking && lvl == SE3TRACKING_MIN_LEVEL)));
					if(buf_warped_size < MIN_GOODPERALL_PIXEL_ABSMIN* (width>>lvl)*(height>>lvl))
					{
						diverged = true;
						trackingWasGood = false;
						return SE3();
					}

					float error = callOptimized(calcWeightsAndResidual,(new_referenceToFrame));
					numCalcResidualCalls[lvl]++;


					// accept inc?
					if(error < lastErr)
					{
						// accept inc
						referenceToFrame = new_referenceToFrame;
						if(useAffineLightningEstimation)
						{
							affineEstimation_a = affineEstimation_a_lastIt;
							affineEstimation_b = affineEstimation_b_lastIt;
						}


						if(enablePrintDebugInfo && printTrackingIterationInfo)
						{
							// debug output
							printf("(%d-%d): ACCEPTED increment of %f with lambda %.1f, residual: %f -> %f\n",
									lvl,iteration, sqrt(inc.dot(inc)), LM_lambda, lastErr, error);

							printf("         p=%.4f %.4f %.4f %.4f %.4f %.4f\n",
									referenceToFrame.log()[0],referenceToFrame.log()[1],referenceToFrame.log()[2],
									referenceToFrame.log()[3],referenceToFrame.log()[4],referenceToFrame.log()[5]);
						}

						// converged?
						if(error / lastErr > settings.convergenceEps[lvl])
						{
							if(enablePrintDebugInfo && printTrackingIterationInfo)
							{
								printf("(%d-%d): FINISHED pyramid level (last residual reduction too small).\n",
										lvl,iteration);
							}
							iteration = settings.maxItsPerLvl[lvl];
						}

						last_residual = lastErr = error;


						if(LM_lambda <= 0.2)
							LM_lambda = 0;
						else
							LM_lambda *= settings.lambdaSuccessFac;

						break;
					}
					else
					{
						if(enablePrintDebugInfo && printTrackingIterationInfo)
						{
							printf("(%d-%d): REJECTED increment of %f with lambda %.1f, (residual: %f -> %f)\n",
									lvl,iteration, sqrt(inc.dot(inc)), LM_lambda, lastErr, error);
						}

						if(!(inc.dot(inc) > settings.stepSizeMin[lvl]))
						{
							if(enablePrintDebugInfo && printTrackingIterationInfo)
							{
								printf("(%d-%d): FINISHED pyramid level (stepsize too small).\n",
										lvl,iteration);
							}
							iteration = settings.maxItsPerLvl[lvl];
							break;
						}

						if(LM_lambda == 0)
							LM_lambda = 0.2;
						else
							LM_lambda *= std::pow(settings.lambdaFailFac, incTry);
					}
				}
			}
		}
	}
	

	

///
	

	// //============check idepth===============
	// float sum_temp = 0;
	// Eigen::Vector2f* VarDataPT = reference->colorAndVarData[1];
	// // if(frame->id()<50)
	// // {
	// // 	for (int id = 0; id < reference->numData[1];id++)
	// // 	{
	// // 		sum_temp += (*VarDataPT)[1];
	// // 	}
	// // 	cout << "numData: " << reference->numData[1] << endl;
	// // 	cout << "逆深度方差：" << sum_temp / reference->numData[1] << endl;
	// // }
	
	// for (int id = 0; id < reference->numData[1];id++)
	// {
	// 	sum_temp += (*VarDataPT)[1];
	// }
	
	// if(sum_temp/reference->numData[1]<0.01)
	// {
	// 	//cout << "联合优化起始帧 : " << frame->id() << endl;
	// 	//================联合优化================
	// 	// ============ 计算重投影误差 ===========
	// 	ProjectionResiduals(reference, frame, referenceToFrame);
	// 	//std::cout<<"keypoints' matches: "<<reference->keyframe->nmatches<<endl;
	// 	reference->makePointCloud(0);
	// 	callOptimized(calcResidualAndBuffers, (reference->posData[0], reference->colorAndVarData[0], 0, reference->numData[0], frame, referenceToFrame, 0, plotTracking));
	// 	//cout << "buf_warped_size: " << buf_warped_size << endl;
	// 	float lastUnionErr = calcProjectWeights() + callOptimized(calcWeightsAndResidual, (referenceToFrame)); //初始化联合优化误差
	// 	//cout << "error: " << error << endl;
	// 	//cout << "lastUnionErr: " << lastUnionErr << endl;
	// 	//cout << "重投影误差：" << calcProjectWeights() << endl;
	// 	//cout << "灰度误差：" << calcWeightsAndResidual(referenceToFrame) << endl;

	// 	float LM_lambda = settings.lambdaInitial[0];

	// 	for (int iteration = 0; iteration < settings.maxItsPerLvl[0]; iteration++) //迭代10次
	// 	{

	// 		calculateUnionWarpUpdate(ls); ////计算雅可比以及最小二乘系数

	// 		int incTry = 0;
	// 		while (true)
	// 		{
	// 			// solve LS system with current lambda, 求解LM
	// 			Vector6 b = -ls.b - ls.pro_b;
	// 			Matrix6x6 A = ls.A + ls.pro_A;
	// 			for (int i = 0; i < 6; i++)
	// 				A(i, i) *= 1 + LM_lambda;
	// 			Vector6 inc = A.ldlt().solve(b);
	// 			incTry++;

	// 			// apply increment. pretty sure this way round is correct, but hard to test.
	// 			Sophus::SE3f new_referenceToFrame = Sophus::SE3f::exp((inc)) * referenceToFrame;
	// 			//Sophus::SE3f new_referenceToFrame = referenceToFrame * Sophus::SE3f::exp((inc));

	// 			// re-evaluate residual
	// 			ProjectionResiduals(reference, frame, referenceToFrame);
	// 			callOptimized(calcResidualAndBuffers, (reference->posData[0], reference->colorAndVarData[0], 0, reference->numData[0], frame, referenceToFrame, 0, plotTracking));
	// 			if (buf_warped_size < MIN_GOODPERALL_PIXEL_ABSMIN * width * height)
	// 			{
	// 				cout << "没有足够的灰度像素投影到当前帧！！！！！！！！！！" << endl;
	// 				diverged = true;
	// 				trackingWasGood = false;
	// 				return SE3();
	// 			}

	// 			float error = calcProjectWeights() + callOptimized(calcWeightsAndResidual, (new_referenceToFrame));

	// 			//cout << "重投影误差：" << calcProjectWeights() << endl;
	// 			//cout << "灰度误差：" << calcWeightsAndResidual(new_referenceToFrame) << endl;

	// 			// accept inc?
	// 			if (error < lastUnionErr)
	// 			{
	// 				// accept inc
	// 				referenceToFrame = new_referenceToFrame;
	// 				if (useAffineLightningEstimation)
	// 				{
	// 					affineEstimation_a = affineEstimation_a_lastIt;
	// 					affineEstimation_b = affineEstimation_b_lastIt;
	// 				}

	// 				if (false) //(enablePrintDebugInfo && printTrackingIterationInfo)
	// 				{
	// 					// debug output
	// 					printf("(%d-%d): ACCEPTED increment of %f with lambda %.1f, residual: %f -> %f\n",
	// 						   0, iteration, sqrt(inc.dot(inc)), LM_lambda, lastUnionErr, error);

	// 					printf("         p=%.4f %.4f %.4f %.4f %.4f %.4f\n",
	// 						   referenceToFrame.log()[0], referenceToFrame.log()[1], referenceToFrame.log()[2],
	// 						   referenceToFrame.log()[3], referenceToFrame.log()[4], referenceToFrame.log()[5]);
	// 				}

	// 				// converged?
	// 				if (error / lastUnionErr > settings.convergenceEps[0])//变化小于0.001即判定收敛
	// 				{
	// 					if (false) // (enablePrintDebugInfo && printTrackingIterationInfo)
	// 					{
	// 						printf("(%d-%d): FINISHED pyramid level (last residual reduction too small).\n",
	// 							   0, iteration);
	// 					}
	// 					iteration = settings.maxItsPerLvl[0];
	// 				}

	// 				// last_residual = lastUnionErr = error;
	// 				lastUnionErr = error;

	// 				if (LM_lambda <= 0.2)
	// 					LM_lambda = 0;
	// 				else
	// 					LM_lambda *= settings.lambdaSuccessFac;

	// 				break;
	// 			}
	// 			else
	// 			{
	// 				if (true) //(enablePrintDebugInfo && printTrackingIterationInfo)
	// 				{
	// 					printf("(%d-%d): REJECTED increment of %f with lambda %.1f, (residual: %f -> %f)\n",
	// 						   0, iteration, sqrt(inc.dot(inc)), LM_lambda, lastUnionErr, error);
	// 				}

	// 				if (!(inc.dot(inc) > settings.stepSizeMin[0]))
	// 				{
	// 					if (true) //(enablePrintDebugInfo && printTrackingIterationInfo)
	// 					{
	// 						printf("(%d-%d): FINISHED pyramid level (stepsize too small).\n",
	// 							   0, iteration);
	// 					}
	// 					iteration = settings.maxItsPerLvl[0];
	// 					break;
	// 				}

	// 				if (LM_lambda == 0)
	// 					LM_lambda = 0.2;
	// 				else
	// 					LM_lambda *= std::pow(settings.lambdaFailFac, incTry);
	// 			}
	// 		}
	// 	}

	// }
///
	if(plotTracking)
		Util::displayImage("TrackingResidual", debugImageResiduals, false);


	if(enablePrintDebugInfo && printTrackingIterationInfo)
	{
		printf("Tracking: ");
			for(int lvl=PYRAMID_LEVELS-1;lvl >= 0;lvl--)
			{
				printf("lvl %d: %d (%d); ",
					lvl,
					numCalcResidualCalls[lvl],
					numCalcWarpUpdateCalls[lvl]);
			}

		printf("\n");
	}

	saveAllTrackingStagesInternal = false;

	lastResidual = last_residual;

	trackingWasGood = !diverged
			&& lastGoodCount / (frame->width(SE3TRACKING_MIN_LEVEL)*frame->height(SE3TRACKING_MIN_LEVEL)) > MIN_GOODPERALL_PIXEL
			&& lastGoodCount / (lastGoodCount + lastBadCount) > MIN_GOODPERGOODBAD_PIXEL;

	if(trackingWasGood)
		reference->keyframe->numFramesTrackedOnThis++;

	frame->initialTrackedResidual = lastResidual / pointUsage;
	frame->pose->thisToParent_raw = sim3FromSE3(toSophus(referenceToFrame.inverse()),1);
	frame->pose->trackingParent = reference->keyframe->pose;
	return toSophus(referenceToFrame.inverse());
}




#if defined(ENABLE_SSE)
float SE3Tracker::calcWeightsAndResidualSSE(
		const Sophus::SE3f& referenceToFrame)
{
	const __m128 txs = _mm_set1_ps((float)(referenceToFrame.translation()[0]));
	const __m128 tys = _mm_set1_ps((float)(referenceToFrame.translation()[1]));
	const __m128 tzs = _mm_set1_ps((float)(referenceToFrame.translation()[2]));

	const __m128 zeros = _mm_set1_ps(0.0f);
	const __m128 ones = _mm_set1_ps(1.0f);


	const __m128 depthVarFacs = _mm_set1_ps((float)settings.var_weight);// float depthVarFac = var_weight;	// the depth var is over-confident. this is a constant multiplier to remedy that.... HACK
	const __m128 sigma_i2s = _mm_set1_ps((float)cameraPixelNoise2);


	const __m128 huber_res_ponlys = _mm_set1_ps((float)(settings.huber_d/2));

	__m128 sumResP = zeros;


	float sumRes = 0;

	for(int i=0;i<buf_warped_size-3;i+=4)
	{
//		float px = *(buf_warped_x+i);	// x'
//		float py = *(buf_warped_y+i);	// y'
//		float pz = *(buf_warped_z+i);	// z'
//		float d = *(buf_d+i);	// d
//		float rp = *(buf_warped_residual+i); // r_p
//		float gx = *(buf_warped_dx+i);	// \delta_x I
//		float gy = *(buf_warped_dy+i);  // \delta_y I
//		float s = depthVarFac * *(buf_idepthVar+i);	// \sigma_d^2


		// calc dw/dd (first 2 components):
		__m128 pzs = _mm_load_ps(buf_warped_z+i);	// z'
		__m128 pz2ds = _mm_rcp_ps(_mm_mul_ps(_mm_mul_ps(pzs, pzs), _mm_load_ps(buf_d+i)));  // 1 / (z' * z' * d)
		__m128 g0s = _mm_sub_ps(_mm_mul_ps(pzs, txs), _mm_mul_ps(_mm_load_ps(buf_warped_x+i), tzs));
		g0s = _mm_mul_ps(g0s,pz2ds); //float g0 = (tx * pz - tz * px) / (pz*pz*d);

		 //float g1 = (ty * pz - tz * py) / (pz*pz*d);
		__m128 g1s = _mm_sub_ps(_mm_mul_ps(pzs, tys), _mm_mul_ps(_mm_load_ps(buf_warped_y+i), tzs));
		g1s = _mm_mul_ps(g1s,pz2ds);

		 // float drpdd = gx * g0 + gy * g1;	// ommitting the minus
		__m128 drpdds = _mm_add_ps(
				_mm_mul_ps(g0s, _mm_load_ps(buf_warped_dx+i)),
				_mm_mul_ps(g1s, _mm_load_ps(buf_warped_dy+i)));

		 //float w_p = 1.0f / (sigma_i2 + s * drpdd * drpdd);
		__m128 w_ps = _mm_rcp_ps(_mm_add_ps(sigma_i2s,
				_mm_mul_ps(drpdds,
						_mm_mul_ps(drpdds,
								_mm_mul_ps(depthVarFacs,
										_mm_load_ps(buf_idepthVar+i))))));


		//float weighted_rp = fabs(rp*sqrtf(w_p));
		__m128 weighted_rps = _mm_mul_ps(_mm_load_ps(buf_warped_residual+i),
				_mm_sqrt_ps(w_ps));
		weighted_rps = _mm_max_ps(weighted_rps, _mm_sub_ps(zeros,weighted_rps));


		//float wh = fabs(weighted_rp < huber_res_ponly ? 1 : huber_res_ponly / weighted_rp);
		__m128 whs = _mm_cmplt_ps(weighted_rps, huber_res_ponlys);	// bitmask 0xFFFFFFFF for 1, 0x000000 for huber_res_ponly / weighted_rp
		whs = _mm_or_ps(
				_mm_and_ps(whs, ones),
				_mm_andnot_ps(whs, _mm_mul_ps(huber_res_ponlys, _mm_rcp_ps(weighted_rps))));




		// sumRes.sumResP += wh * w_p * rp*rp;
		if(i+3 < buf_warped_size)
			sumResP = _mm_add_ps(sumResP,
					_mm_mul_ps(whs, _mm_mul_ps(weighted_rps, weighted_rps)));

		// *(buf_weight_p+i) = wh * w_p;
		_mm_store_ps(buf_weight_p+i, _mm_mul_ps(whs, w_ps) );
	}
	sumRes = SSEE(sumResP,0) + SSEE(sumResP,1) + SSEE(sumResP,2) + SSEE(sumResP,3);

	return sumRes / ((buf_warped_size >> 2)<<2);
}
#endif



#if defined(ENABLE_NEON)
float SE3Tracker::calcWeightsAndResidualNEON(
		const Sophus::SE3f& referenceToFrame)
{
	float tx = referenceToFrame.translation()[0];
	float ty = referenceToFrame.translation()[1];
	float tz = referenceToFrame.translation()[2];


	float constants[] = {
		tx, ty, tz, settings.var_weight,
		cameraPixelNoise2, settings.huber_d/2, -1, -1 // last values are currently unused
	};
	// This could also become a constant if one register could be made free for it somehow
	float cutoff_res_ponly4[4] = {10000, 10000, 10000, 10000}; // removed
	float* cur_buf_warped_z = buf_warped_z;
	float* cur_buf_warped_x = buf_warped_x;
	float* cur_buf_warped_y = buf_warped_y;
	float* cur_buf_warped_dx = buf_warped_dx;
	float* cur_buf_warped_dy = buf_warped_dy;
	float* cur_buf_warped_residual = buf_warped_residual;
	float* cur_buf_d = buf_d;
	float* cur_buf_idepthVar = buf_idepthVar;
	float* cur_buf_weight_p = buf_weight_p;
	int loop_count = buf_warped_size / 4;
	int remaining = buf_warped_size - 4 * loop_count;
	float sum_vector[] = {0, 0, 0, 0};
	
	float sumRes=0;

	
#ifdef DEBUG
	loop_count = 0;
	remaining = buf_warped_size;
#else
	if (loop_count > 0)
	{
		__asm__ __volatile__
		(
			// Extract constants
			"vldmia   %[constants], {q8-q9}              \n\t" // constants(q8-q9)
			"vdup.32  q13, d18[0]                        \n\t" // extract sigma_i2 x 4 to q13
			"vdup.32  q14, d18[1]                        \n\t" // extract huber_res_ponly x 4 to q14
			//"vdup.32  ???, d19[0]                        \n\t" // extract cutoff_res_ponly x 4 to ???
			"vdup.32  q9, d16[0]                         \n\t" // extract tx x 4 to q9, overwrite!
			"vdup.32  q10, d16[1]                        \n\t" // extract ty x 4 to q10
			"vdup.32  q11, d17[0]                        \n\t" // extract tz x 4 to q11
			"vdup.32  q8, d17[1]                         \n\t" // extract depthVarFac x 4 to q8, overwrite!
			
			"veor     q15, q15, q15                      \n\t" // set sumRes.sumResP(q15) to zero (by xor with itself)
			".loopcalcWeightsAndResidualNEON:            \n\t"
			
				"vldmia   %[buf_idepthVar]!, {q7}           \n\t" // s(q7)
				"vldmia   %[buf_warped_z]!, {q2}            \n\t" // pz(q2)
				"vldmia   %[buf_d]!, {q3}                   \n\t" // d(q3)
				"vldmia   %[buf_warped_x]!, {q0}            \n\t" // px(q0)
				"vldmia   %[buf_warped_y]!, {q1}            \n\t" // py(q1)
				"vldmia   %[buf_warped_residual]!, {q4}     \n\t" // rp(q4)
				"vldmia   %[buf_warped_dx]!, {q5}           \n\t" // gx(q5)
				"vldmia   %[buf_warped_dy]!, {q6}           \n\t" // gy(q6)
		
				"vmul.f32 q7, q7, q8                        \n\t" // s *= depthVarFac
				"vmul.f32 q12, q2, q2                       \n\t" // pz*pz (q12, temp)
				"vmul.f32 q3, q12, q3                       \n\t" // pz*pz*d (q3)
		
				"vrecpe.f32 q3, q3                          \n\t" // 1/(pz*pz*d) (q3)
				"vmul.f32 q12, q9, q2                       \n\t" // tx*pz (q12)
				"vmls.f32 q12, q11, q0                      \n\t" // tx*pz - tz*px (q12) [multiply and subtract] {free: q0}
				"vmul.f32 q0, q10, q2                       \n\t" // ty*pz (q0) {free: q2}
				"vmls.f32 q0, q11, q1                       \n\t" // ty*pz - tz*py (q0) {free: q1}
				"vmul.f32 q12, q12, q3                      \n\t" // g0 (q12)
				"vmul.f32 q0, q0, q3                        \n\t" // g1 (q0)
		
				"vmul.f32 q12, q12, q5                      \n\t" // gx * g0 (q12) {free: q5}
				"vldmia %[cutoff_res_ponly4], {q5}          \n\t" // cutoff_res_ponly (q5), load for later
				"vmla.f32 q12, q6, q0                       \n\t" // drpdd = gx * g0 + gy * g1 (q12) {free: q6, q0}
				
				"vmov.f32 q1, #1.0                          \n\t" // 1.0 (q1), will be used later
		
				"vmul.f32 q12, q12, q12                     \n\t" // drpdd*drpdd (q12)
				"vmul.f32 q12, q12, q7                      \n\t" // s*drpdd*drpdd (q12)
				"vadd.f32 q12, q12, q13                     \n\t" // sigma_i2 + s*drpdd*drpdd (q12)
				"vrecpe.f32 q12, q12                        \n\t" // w_p = 1/(sigma_i2 + s*drpdd*drpdd) (q12) {free: q7}
		
				// float weighted_rp = fabs(rp*sqrtf(w_p));
				"vrsqrte.f32 q7, q12                        \n\t" // 1 / sqrtf(w_p) (q7)
				"vrecpe.f32 q7, q7                          \n\t" // sqrtf(w_p) (q7)
				"vmul.f32 q7, q7, q4                        \n\t" // rp*sqrtf(w_p) (q7)
				"vabs.f32 q7, q7                            \n\t" // weighted_rp (q7)
		
				// float wh = fabs(weighted_rp < huber_res_ponly ? 1 : huber_res_ponly / weighted_rp);
				"vrecpe.f32 q6, q7                          \n\t" // 1 / weighted_rp (q6)
				"vmul.f32 q6, q6, q14                       \n\t" // huber_res_ponly / weighted_rp (q6)
				"vclt.f32 q0, q7, q14                       \n\t" // weighted_rp < huber_res_ponly ? all bits 1 : all bits 0 (q0)
				"vbsl     q0, q1, q6                        \n\t" // sets elements in q0 to 1(q1) where above condition is true, and to q6 where it is false {free: q6}
				"vabs.f32 q0, q0                            \n\t" // wh (q0)
		
				// sumRes.sumResP += wh * w_p * rp*rp
				"vmul.f32 q4, q4, q4                        \n\t" // rp*rp (q4)
				"vmul.f32 q4, q4, q12                       \n\t" // w_p*rp*rp (q4)
				"vmla.f32 q15, q4, q0                       \n\t" // sumRes.sumResP += wh*w_p*rp*rp (q15) {free: q4}
				
				// if(weighted_rp > cutoff_res_ponly)
				//     wh = 0;
				// *(buf_weight_p+i) = wh * w_p;
				"vcle.f32 q4, q7, q5                        \n\t" // mask in q4: ! (weighted_rp > cutoff_res_ponly)
				"vmul.f32 q0, q0, q12                       \n\t" // wh * w_p (q0)
				"vand     q0, q0, q4                        \n\t" // set q0 to 0 where condition for q4 failed (i.e. weighted_rp > cutoff_res_ponly)
				"vstmia   %[buf_weight_p]!, {q0}            \n\t"
				
			"subs     %[loop_count], %[loop_count], #1    \n\t"
			"bne      .loopcalcWeightsAndResidualNEON     \n\t"
				
			"vstmia   %[sum_vector], {q15}                \n\t"

		: /* outputs */ [buf_warped_z]"+&r"(cur_buf_warped_z),
						[buf_warped_x]"+&r"(cur_buf_warped_x),
						[buf_warped_y]"+&r"(cur_buf_warped_y),
						[buf_warped_dx]"+&r"(cur_buf_warped_dx),
						[buf_warped_dy]"+&r"(cur_buf_warped_dy),
						[buf_d]"+&r"(cur_buf_d),
						[buf_warped_residual]"+&r"(cur_buf_warped_residual),
						[buf_idepthVar]"+&r"(cur_buf_idepthVar),
						[buf_weight_p]"+&r"(cur_buf_weight_p),
						[loop_count]"+&r"(loop_count)
		: /* inputs  */ [constants]"r"(constants),
						[cutoff_res_ponly4]"r"(cutoff_res_ponly4),
						[sum_vector]"r"(sum_vector)
		: /* clobber */ "memory", "cc",
						"q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7", "q8", "q9", "q10", "q11", "q12", "q13", "q14", "q15"
		);
		
		sumRes += sum_vector[0] + sum_vector[1] + sum_vector[2] + sum_vector[3];
	}
#endif

	for(int i=buf_warped_size-remaining; i<buf_warped_size; i++)
	{
		float px = *(buf_warped_x+i);	// x'
		float py = *(buf_warped_y+i);	// y'
		float pz = *(buf_warped_z+i);	// z'
		float d = *(buf_d+i);	// d
		float rp = *(buf_warped_residual+i); // r_p
		float gx = *(buf_warped_dx+i);	// \delta_x I
		float gy = *(buf_warped_dy+i);  // \delta_y I
		float s = settings.var_weight * *(buf_idepthVar+i);	// \sigma_d^2


		// calc dw/dd (first 2 components):
		float g0 = (tx * pz - tz * px) / (pz*pz*d);
		float g1 = (ty * pz - tz * py) / (pz*pz*d);


		// calc w_p
		float drpdd = gx * g0 + gy * g1;	// ommitting the minus
		float w_p = 1.0f / (cameraPixelNoise2 + s * drpdd * drpdd);
		float weighted_rp = fabs(rp*sqrtf(w_p));

		float wh = fabs(weighted_rp < (settings.huber_d/2) ? 1 : (settings.huber_d/2) / weighted_rp);

		sumRes += wh * w_p * rp*rp;

		*(buf_weight_p+i) = wh * w_p;
	}

	return sumRes / buf_warped_size;
}
#endif

float SE3Tracker::calcWeightsAndResidual(
		const Sophus::SE3f& referenceToFrame)
{
	float tx = referenceToFrame.translation()[0];
	float ty = referenceToFrame.translation()[1];
	float tz = referenceToFrame.translation()[2];

	float sumRes = 0;

	for(int i=0;i<buf_warped_size;i++)
	{
		float px = *(buf_warped_x+i);	// x'
		float py = *(buf_warped_y+i);	// y'
		float pz = *(buf_warped_z+i);	// z'
		float d = *(buf_d+i);	// d
		float rp = *(buf_warped_residual+i); // r_p
		float gx = *(buf_warped_dx+i);	// \delta_x I
		float gy = *(buf_warped_dy+i);  // \delta_y I
		float s = settings.var_weight * *(buf_idepthVar+i);	// \sigma_d^2


		// calc dw/dd (first 2 components):
		float g0 = (tx * pz - tz * px) / (pz*pz*d);
		float g1 = (ty * pz - tz * py) / (pz*pz*d);


		// calc w_p
		float drpdd = gx * g0 + gy * g1;	// ommitting the minus
		float w_p = 1.0f / ((cameraPixelNoise2) + s * drpdd * drpdd);

		float weighted_rp = fabs(rp*sqrtf(w_p));

		float wh = fabs(weighted_rp < (settings.huber_d/2) ? 1 : (settings.huber_d/2) / weighted_rp);

		sumRes += wh * w_p * rp*rp;


		*(buf_weight_p+i) = wh * w_p;
	}

	return sumRes / buf_warped_size;
}

float SE3Tracker::calcProjectWeights()//计算重投影误差归一化参数
{
	float sumRes = 0;

	// float K = 261 / (1 + exp((float)(300 - nmatches) / 40));
	// cout << "K: " << K << endl;
	//float K = 6;
	for (int i = 0; i < project_buf_warped_size; i++)
	{
		float rp_x = *(project_buf_warped_residual_x+i); // r_p
		float rp_y = *(project_buf_warped_residual_y+i); // r_p
		float s = settings.var_weight * *(project_buf_idepthVar+i);	// \sigma_d^2, var_weight=1.0

		float rp = sqrtf(rp_x * rp_x + rp_y * rp_y);
		
		// calc w_p
		float w_p = (1.0f / (s + 0.0001)) / (1.0f/(minidepthVar+0.0001));//权重中的深度不确定部分

		float weighted_rp = fabs(rp*sqrtf(w_p));

		//cout << weighted_rp << " ";

		float wh = fabs(weighted_rp < (settings.huber_d/2) ? 1 : (settings.huber_d/2) / weighted_rp);//huber_d=3

		sumRes += wh * w_p * rp*rp;


		*(project_buf_weight_p+i) = wh * w_p;// * K;
	}

	return sumRes / project_buf_warped_size;
}


void SE3Tracker::calcResidualAndBuffers_debugStart()
{
	if(plotTrackingIterationInfo || saveAllTrackingStagesInternal)
	{
		int other = saveAllTrackingStagesInternal ? 255 : 0;
		fillCvMat(&debugImageResiduals,cv::Vec3b(other,other,255));
		fillCvMat(&debugImageWeights,cv::Vec3b(other,other,255));
		fillCvMat(&debugImageOldImageSource,cv::Vec3b(other,other,255));
		fillCvMat(&debugImageOldImageWarped,cv::Vec3b(other,other,255));
	}
}

void SE3Tracker::calcResidualAndBuffers_debugFinish(int w)
{
	if(plotTrackingIterationInfo)
	{
		Util::displayImage( "Weights", debugImageWeights );
		Util::displayImage( "second_frame", debugImageSecondFrame );
		Util::displayImage( "Intensities of second_frame at transformed positions", debugImageOldImageSource );
		Util::displayImage( "Intensities of second_frame at pointcloud in first_frame", debugImageOldImageWarped );
		Util::displayImage( "Residuals", debugImageResiduals );


		// wait for key and handle it
		bool looping = true;
		while(looping)
		{
			int k = Util::waitKey(1);
			if(k == -1)
			{
				if(autoRunWithinFrame)
					break;
				else
					continue;
			}

			char key = k;
			if(key == ' ')
				looping = false;
			else
				handleKey(k);
		}
	}

	if(saveAllTrackingStagesInternal)
	{
		char charbuf[500];

		snprintf(charbuf,500,"save/%sresidual-%d-%d.png",packagePath.c_str(),w,iterationNumber);
		cv::imwrite(charbuf,debugImageResiduals);

		snprintf(charbuf,500,"save/%swarped-%d-%d.png",packagePath.c_str(),w,iterationNumber);
		cv::imwrite(charbuf,debugImageOldImageWarped);

		snprintf(charbuf,500,"save/%sweights-%d-%d.png",packagePath.c_str(),w,iterationNumber);
		cv::imwrite(charbuf,debugImageWeights);

		printf("saved three images for lvl %d, iteration %d\n",w,iterationNumber);
	}
}

#if defined(ENABLE_SSE)
float SE3Tracker::calcResidualAndBuffersSSE(
		const Eigen::Vector3f* refPoint,
		const Eigen::Vector2f* refColVar,
		int* idxBuf,
		int refNum,
		Frame* frame,
		const Sophus::SE3f& referenceToFrame,
		int level,
		bool plotResidual)
{
	return calcResidualAndBuffers(refPoint, refColVar, idxBuf, refNum, frame, referenceToFrame, level, plotResidual);
}
#endif

#if defined(ENABLE_NEON)
float SE3Tracker::calcResidualAndBuffersNEON(
		const Eigen::Vector3f* refPoint,
		const Eigen::Vector2f* refColVar,
		int* idxBuf,
		int refNum,
		Frame* frame,
		const Sophus::SE3f& referenceToFrame,
		int level,
		bool plotResidual)
{
	return calcResidualAndBuffers(refPoint, refColVar, idxBuf, refNum, frame, referenceToFrame, level, plotResidual);
}
#endif


float SE3Tracker::calcResidualAndBuffers(
		const Eigen::Vector3f* refPoint,
		const Eigen::Vector2f* refColVar,
		int* idxBuf,
		int refNum,
		Frame* frame,
		const Sophus::SE3f& referenceToFrame,
		int level,
		bool plotResidual)
{
	calcResidualAndBuffers_debugStart();

	if(plotResidual)
		debugImageResiduals.setTo(0);


	int w = frame->width(level);
	int h = frame->height(level);
	Eigen::Matrix3f KLvl = frame->K(level);
	float fx_l = KLvl(0,0);
	float fy_l = KLvl(1,1);
	float cx_l = KLvl(0,2);
	float cy_l = KLvl(1,2);

	buf_fx = fx_l;
	buf_fy = fy_l;

	Eigen::Matrix3f rotMat = referenceToFrame.rotationMatrix();
	Eigen::Vector3f transVec = referenceToFrame.translation();
	
	const Eigen::Vector3f* refPoint_max = refPoint + refNum;


	const Eigen::Vector4f* frame_gradients = frame->gradients(level);

	int idx=0;

	float sumResUnweighted = 0;

	bool* isGoodOutBuffer = idxBuf != 0 ? frame->refPixelWasGood() : 0;

	int goodCount = 0;
	int badCount = 0;

	float sumSignedRes = 0;



	float sxx=0,syy=0,sx=0,sy=0,sw=0;

	float usageCount = 0;

	for(;refPoint<refPoint_max; refPoint++, refColVar++, idxBuf++)
	{

		Eigen::Vector3f Wxp = rotMat * (*refPoint) + transVec;
		float u_new = (Wxp[0]/Wxp[2])*fx_l + cx_l;
		float v_new = (Wxp[1]/Wxp[2])*fy_l + cy_l;

		// step 1a: coordinates have to be in image:
		// (inverse test to exclude NANs)
		if(!(u_new > 1 && v_new > 1 && u_new < w-2 && v_new < h-2))
		{
			if(isGoodOutBuffer != 0)
				isGoodOutBuffer[*idxBuf] = false;
			continue;
		}

		Eigen::Vector3f resInterp = getInterpolatedElement43(frame_gradients, u_new, v_new, w);

		float c1 = affineEstimation_a * (*refColVar)[0] + affineEstimation_b;
		float c2 = resInterp[2];
		float residual = c1 - c2;

		float weight = fabsf(residual) < 5.0f ? 1 : 5.0f / fabsf(residual);
		sxx += c1*c1*weight;
		syy += c2*c2*weight;
		sx += c1*weight;
		sy += c2*weight;
		sw += weight;

		bool isGood = residual*residual / (MAX_DIFF_CONSTANT + MAX_DIFF_GRAD_MULT*(resInterp[0]*resInterp[0] + resInterp[1]*resInterp[1])) < 1;

		if(isGoodOutBuffer != 0)
			isGoodOutBuffer[*idxBuf] = isGood;

		*(buf_warped_x+idx) = Wxp(0);
		*(buf_warped_y+idx) = Wxp(1);
		*(buf_warped_z+idx) = Wxp(2);

		*(buf_warped_dx+idx) = fx_l * resInterp[0];
		*(buf_warped_dy+idx) = fy_l * resInterp[1];
		*(buf_warped_residual+idx) = residual;

		*(buf_d+idx) = 1.0f / (*refPoint)[2];
		*(buf_idepthVar+idx) = (*refColVar)[1];
		idx++;


		if(isGood)
		{
			sumResUnweighted += residual*residual;
			sumSignedRes += residual;
			goodCount++;
		}
		else
			badCount++;

		float depthChange = (*refPoint)[2] / Wxp[2];	// if depth becomes larger: pixel becomes "smaller", hence count it less.
		usageCount += depthChange < 1 ? depthChange : 1;


		// DEBUG STUFF
		if(plotTrackingIterationInfo || plotResidual)
		{
			// for debug plot only: find x,y again.
			// horribly inefficient, but who cares at this point...
			Eigen::Vector3f point = KLvl * (*refPoint);
			int x = point[0] / point[2] + 0.5f;
			int y = point[1] / point[2] + 0.5f;

			if(plotTrackingIterationInfo)
			{
				setPixelInCvMat(&debugImageOldImageSource,getGrayCvPixel((float)resInterp[2]),u_new+0.5,v_new+0.5,(width/w));
				setPixelInCvMat(&debugImageOldImageWarped,getGrayCvPixel((float)resInterp[2]),x,y,(width/w));
			}
			if(isGood)
				setPixelInCvMat(&debugImageResiduals,getGrayCvPixel(residual+128),x,y,(width/w));
			else
				setPixelInCvMat(&debugImageResiduals,cv::Vec3b(0,0,255),x,y,(width/w));

		}
	}

	buf_warped_size = idx;

	pointUsage = usageCount / (float)refNum;
	lastGoodCount = goodCount;
	lastBadCount = badCount;
	lastMeanRes = sumSignedRes / goodCount;

	affineEstimation_a_lastIt = sqrtf((syy - sy*sy/sw) / (sxx - sx*sx/sw));
	affineEstimation_b_lastIt = (sy - affineEstimation_a_lastIt*sx)/sw;

	calcResidualAndBuffers_debugFinish(w);

	return sumResUnweighted / goodCount;
}


#if defined(ENABLE_SSE)
Vector6 SE3Tracker::calculateWarpUpdateSSE(
		NormalEquationsLeastSquares &ls)
{
	ls.initialize(width*height);

//	printf("wupd SSE\n");
	for(int i=0;i<buf_warped_size-3;i+=4)
	{
		Vector6 v1,v2,v3,v4;
		__m128 val1, val2, val3, val4;

		// redefine pz
		__m128 pz = _mm_load_ps(buf_warped_z+i);
		pz = _mm_rcp_ps(pz);						// pz := 1/z


		__m128 gx = _mm_load_ps(buf_warped_dx+i);
		val1 = _mm_mul_ps(pz, gx);			// gx / z => SET [0]
		//v[0] = z*gx;
		v1[0] = SSEE(val1,0);
		v2[0] = SSEE(val1,1);
		v3[0] = SSEE(val1,2);
		v4[0] = SSEE(val1,3);




		__m128 gy = _mm_load_ps(buf_warped_dy+i);
		val1 = _mm_mul_ps(pz, gy);					// gy / z => SET [1]
		//v[1] = z*gy;
		v1[1] = SSEE(val1,0);
		v2[1] = SSEE(val1,1);
		v3[1] = SSEE(val1,2);
		v4[1] = SSEE(val1,3);


		__m128 px = _mm_load_ps(buf_warped_x+i);
		val1 = _mm_mul_ps(px, gy);
		val1 = _mm_mul_ps(val1, pz);	//  px * gy * z
		__m128 py = _mm_load_ps(buf_warped_y+i);
		val2 = _mm_mul_ps(py, gx);
		val2 = _mm_mul_ps(val2, pz);	//  py * gx * z
		val1 = _mm_sub_ps(val1, val2);  // px * gy * z - py * gx * z => SET [5]
		//v[5] = -py * z * gx +  px * z * gy;
		v1[5] = SSEE(val1,0);
		v2[5] = SSEE(val1,1);
		v3[5] = SSEE(val1,2);
		v4[5] = SSEE(val1,3);


		// redefine pz
		pz = _mm_mul_ps(pz,pz); 		// pz := 1/(z*z)

		// will use these for the following calculations a lot.
		val1 = _mm_mul_ps(px, gx);
		val1 = _mm_mul_ps(val1, pz);		// px * z_sqr * gx
		val2 = _mm_mul_ps(py, gy);
		val2 = _mm_mul_ps(val2, pz);		// py * z_sqr * gy


		val3 = _mm_add_ps(val1, val2);
		val3 = _mm_sub_ps(_mm_setr_ps(0,0,0,0),val3);	//-px * z_sqr * gx -py * z_sqr * gy
		//v[2] = -px * z_sqr * gx -py * z_sqr * gy;	=> SET [2]
		v1[2] = SSEE(val3,0);
		v2[2] = SSEE(val3,1);
		v3[2] = SSEE(val3,2);
		v4[2] = SSEE(val3,3);


		val3 = _mm_mul_ps(val1, py); // px * z_sqr * gx * py
		val4 = _mm_add_ps(gy, val3); // gy + px * z_sqr * gx * py
		val3 = _mm_mul_ps(val2, py); // py * py * z_sqr * gy
		val4 = _mm_add_ps(val3, val4); // gy + px * z_sqr * gx * py + py * py * z_sqr * gy
		val4 = _mm_sub_ps(_mm_setr_ps(0,0,0,0),val4); //val4 = -val4.
		//v[3] = -px * py * z_sqr * gx +
		//       -py * py * z_sqr * gy +
		//       -gy;		=> SET [3]
		v1[3] = SSEE(val4,0);
		v2[3] = SSEE(val4,1);
		v3[3] = SSEE(val4,2);
		v4[3] = SSEE(val4,3);


		val3 = _mm_mul_ps(val1, px); // px * px * z_sqr * gx
		val4 = _mm_add_ps(gx, val3); // gx + px * px * z_sqr * gx
		val3 = _mm_mul_ps(val2, px); // px * py * z_sqr * gy
		val4 = _mm_add_ps(val4, val3); // gx + px * px * z_sqr * gx + px * py * z_sqr * gy
		//v[4] = px * px * z_sqr * gx +
		//	   px * py * z_sqr * gy +
		//	   gx;				=> SET [4]
		v1[4] = SSEE(val4,0);
		v2[4] = SSEE(val4,1);
		v3[4] = SSEE(val4,2);
		v4[4] = SSEE(val4,3);

		// step 6: integrate into A and b:
		ls.update(v1, *(buf_warped_residual+i+0), *(buf_weight_p+i+0));

		if(i+1>=buf_warped_size) break;
		ls.update(v2, *(buf_warped_residual+i+1), *(buf_weight_p+i+1));

		if(i+2>=buf_warped_size) break;
		ls.update(v3, *(buf_warped_residual+i+2), *(buf_weight_p+i+2));

		if(i+3>=buf_warped_size) break;
		ls.update(v4, *(buf_warped_residual+i+3), *(buf_weight_p+i+3));
	}
	Vector6 result;

	// solve ls
	ls.finish();
	ls.solve(result);

	return result;
}
#endif


#if defined(ENABLE_NEON)
Vector6 SE3Tracker::calculateWarpUpdateNEON(
		NormalEquationsLeastSquares &ls)
{
//	weightEstimator.reset();
//	weightEstimator.estimateDistributionNEON(buf_warped_residual, buf_warped_size);
//	weightEstimator.calcWeightsNEON(buf_warped_residual, buf_warped_weights, buf_warped_size);

	ls.initialize(width*height);
	
	float* cur_buf_warped_z = buf_warped_z;
	float* cur_buf_warped_x = buf_warped_x;
	float* cur_buf_warped_y = buf_warped_y;
	float* cur_buf_warped_dx = buf_warped_dx;
	float* cur_buf_warped_dy = buf_warped_dy;
	Vector6 v1,v2,v3,v4;
	float* v1_ptr;
	float* v2_ptr;
	float* v3_ptr;
	float* v4_ptr;
	for(int i=0;i<buf_warped_size;i+=4)
	{
		v1_ptr = &v1[0];
		v2_ptr = &v2[0];
		v3_ptr = &v3[0];
		v4_ptr = &v4[0];
	
		__asm__ __volatile__
		(
			"vldmia   %[buf_warped_z]!, {q10}            \n\t" // pz(q10)
			"vrecpe.f32 q10, q10                         \n\t" // z(q10)
			
			"vldmia   %[buf_warped_dx]!, {q11}           \n\t" // gx(q11)
			"vmul.f32 q0, q10, q11                       \n\t" // q0 = z*gx // = v[0]
			
			"vldmia   %[buf_warped_dy]!, {q12}           \n\t" // gy(q12)
			"vmul.f32 q1, q10, q12                       \n\t" // q1 = z*gy // = v[1]
			
			"vldmia   %[buf_warped_x]!, {q13}            \n\t" // px(q13)
			"vmul.f32 q5, q13, q12                       \n\t" // q5 = px * gy
			"vmul.f32 q5, q5, q10                        \n\t" // q5 = q5 * z = px * gy * z
			
			"vldmia   %[buf_warped_y]!, {q14}            \n\t" // py(q14)
			"vmul.f32 q3, q14, q11                       \n\t" // q3 = py * gx
			"vmls.f32 q5, q3, q10                        \n\t" // q5 = px * gy * z - py * gx * z // = v[5] (vmls: multiply and subtract from result)
			
			"vmul.f32 q10, q10, q10                      \n\t" // q10 = 1/(pz*pz)
		
			"vmul.f32 q6, q13, q11                       \n\t"
			"vmul.f32 q6, q6, q10                        \n\t" // q6 = val1 in SSE version = px * z_sqr * gx
			
			"vmul.f32 q7, q14, q12                       \n\t"
			"vmul.f32 q7, q7, q10                        \n\t" // q7 = val2 in SSE version = py * z_sqr * gy
			
			"vadd.f32 q2, q6, q7                         \n\t"
			"vneg.f32 q2, q2                             \n\t" // q2 = -px * z_sqr * gx -py * z_sqr * gy // = v[2]
			
			"vmul.f32 q8, q6, q14                        \n\t" // val3(q8) = px * z_sqr * gx * py
			"vadd.f32 q9, q12, q8                        \n\t" // val4(q9) = gy + px * z_sqr * gx * py
			"vmul.f32 q8, q7, q14                        \n\t" // val3(q8) = py * py * z_sqr * gy
			"vadd.f32 q9, q8, q9                         \n\t" // val4(q9) = gy + px * z_sqr * gx * py + py * py * z_sqr * gy
			"vneg.f32 q3, q9                             \n\t" // q3 = v[3]
			
			"vst4.32 {d0[0], d2[0], d4[0], d6[0]}, [%[v1]]! \n\t" // store v[0] .. v[3] for 1st value and inc pointer
			"vst4.32 {d0[1], d2[1], d4[1], d6[1]}, [%[v2]]! \n\t" // store v[0] .. v[3] for 2nd value and inc pointer
			"vst4.32 {d1[0], d3[0], d5[0], d7[0]}, [%[v3]]! \n\t" // store v[0] .. v[3] for 3rd value and inc pointer
			"vst4.32 {d1[1], d3[1], d5[1], d7[1]}, [%[v4]]! \n\t" // store v[0] .. v[3] for 4th value and inc pointer
			
			"vmul.f32 q8, q6, q13                        \n\t" // val3(q8) = px * px * z_sqr * gx
			"vadd.f32 q9, q11, q8                        \n\t" // val4(q9) = gx + px * px * z_sqr * gx
			"vmul.f32 q8, q7, q13                        \n\t" // val3(q8) = px * py * z_sqr * gy
			"vadd.f32 q4, q9, q8                         \n\t" // q4 = v[4]
			
			"vst2.32 {d8[0], d10[0]}, [%[v1]]               \n\t" // store v[4], v[5] for 1st value
			"vst2.32 {d8[1], d10[1]}, [%[v2]]               \n\t" // store v[4], v[5] for 2nd value
			"vst2.32 {d9[0], d11[0]}, [%[v3]]               \n\t" // store v[4], v[5] for 3rd value
			"vst2.32 {d9[1], d11[1]}, [%[v4]]               \n\t" // store v[4], v[5] for 4th value

        : /* outputs */ [buf_warped_z]"+r"(cur_buf_warped_z),
		                [buf_warped_x]"+r"(cur_buf_warped_x),
		                [buf_warped_y]"+r"(cur_buf_warped_y),
		                [buf_warped_dx]"+r"(cur_buf_warped_dx),
		                [buf_warped_dy]"+r"(cur_buf_warped_dy),
		                [v1]"+r"(v1_ptr),
		                [v2]"+r"(v2_ptr),
		                [v3]"+r"(v3_ptr),
		                [v4]"+r"(v4_ptr)
        : /* inputs  */ 
        : /* clobber */ "memory", "cc", // TODO: is cc necessary?
	                    "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7", "q8", "q9", "q10", "q11", "q12", "q13", "q14"
		);
		

		// step 6: integrate into A and b:
		if(!(i+3>=buf_warped_size))
		{
			ls.update(v1, *(buf_warped_residual+i+0), *(buf_weight_p+i+0));
			ls.update(v2, *(buf_warped_residual+i+1), *(buf_weight_p+i+1));
			ls.update(v3, *(buf_warped_residual+i+2), *(buf_weight_p+i+2));
			ls.update(v4, *(buf_warped_residual+i+3), *(buf_weight_p+i+3));
		}
		else
		{
			ls.update(v1, *(buf_warped_residual+i+0), *(buf_weight_p+i+0));

			if(i+1>=buf_warped_size) break;
			ls.update(v2, *(buf_warped_residual+i+1), *(buf_weight_p+i+1));

			if(i+2>=buf_warped_size) break;
			ls.update(v3, *(buf_warped_residual+i+2), *(buf_weight_p+i+2));

			if(i+3>=buf_warped_size) break;
			ls.update(v4, *(buf_warped_residual+i+3), *(buf_weight_p+i+3));
		}
	}
	Vector6 result;

	// solve ls
	ls.finish();
	ls.solve(result);

	return result;
}
#endif


Vector6 SE3Tracker::calculateWarpUpdate(
		NormalEquationsLeastSquares &ls)
{
//	weightEstimator.reset();
//	weightEstimator.estimateDistribution(buf_warped_residual, buf_warped_size);
//	weightEstimator.calcWeights(buf_warped_residual, buf_warped_weights, buf_warped_size);
//
	ls.initialize(width*height);
	for(int i=0;i<buf_warped_size;i++)
	{
		float px = *(buf_warped_x+i);
		float py = *(buf_warped_y+i);
		float pz = *(buf_warped_z+i);
		float r =  *(buf_warped_residual+i);
		float gx = *(buf_warped_dx+i);
		float gy = *(buf_warped_dy+i);
		// step 3 + step 5 comp 6d error vector

		float z = 1.0f / pz;
		float z_sqr = 1.0f / (pz*pz);
		Vector6 v;
		v[0] = z*gx + 0;
		v[1] = 0 +         z*gy;
		v[2] = (-px * z_sqr) * gx +
			  (-py * z_sqr) * gy;
		v[3] = (-px * py * z_sqr) * gx +
			  (-(1.0 + py * py * z_sqr)) * gy;
		v[4] = (1.0 + px * px * z_sqr) * gx +
			  (px * py * z_sqr) * gy;
		v[5] = (-py * z) * gx +
			  (px * z) * gy;

		// step 6: integrate into A and b:
		ls.update(v, r, *(buf_weight_p+i));
	}
	Vector6 result;

	// solve ls
	ls.finish();
	ls.solve(result);

	return result;
}


Vector6 SE3Tracker::calculateUnionWarpUpdate(
		NormalEquationsLeastSquares &ls)
{
//	weightEstimator.reset();
//	weightEstimator.estimateDistribution(buf_warped_residual, buf_warped_size);
//	weightEstimator.calcWeights(buf_warped_residual, buf_warped_weights, buf_warped_size);
//

	ls.initialize(width*height);
	ls.unionopt = true;
	//===========计算灰度误差雅各比矩阵============
	for(int i=0;i<buf_warped_size;i++)
	{
		float px = *(buf_warped_x+i);
		float py = *(buf_warped_y+i);
		float pz = *(buf_warped_z+i);
		float r =  *(buf_warped_residual+i);
		float gx = *(buf_warped_dx+i);
		float gy = *(buf_warped_dy+i);
		// step 3 + step 5 comp 6d error vector

		float z = 1.0f / pz;
		float z_sqr = 1.0f / (pz*pz);
		Vector6 v;
		v[0] = z*gx + 0;
		v[1] = 0 +         z*gy;
		v[2] = (-px * z_sqr) * gx +
			  (-py * z_sqr) * gy;
		v[3] = (-px * py * z_sqr) * gx +
			  (-(1.0 + py * py * z_sqr)) * gy;
		v[4] = (1.0 + px * px * z_sqr) * gx +
			  (px * py * z_sqr) * gy;
		v[5] = (-py * z) * gx +
			  (px * z) * gy;

		// step 6: integrate into A and b:
		ls.update(v, r, *(buf_weight_p+i));
	}
	

	//===========计算几何误差雅各比矩阵============
	for (int i = 0; i < project_buf_warped_size; i++)
	{
		float px = *(project_buf_warped_x+i);
		float py = *(project_buf_warped_y+i);
		float pz = *(project_buf_warped_z+i);
		float rx = *(project_buf_warped_residual_x+i);
		float ry = *(project_buf_warped_residual_y+i);
		float z = 1.0f / pz;
		float z_sqr = 1.0f / (pz*pz);

		Eigen::Matrix<float , 6 ,2> v;
		Eigen::Vector2f res;

		v(0,0) = buf_fx * z;
		v(1,0) = 0;
		v(2,0) = -buf_fx * px * z_sqr;
		v(3,0) = -buf_fx * px * py * z_sqr;
		v(4,0) = buf_fx + buf_fx * px * px * z_sqr;
		v(5,0) = -buf_fx * py * z;

		v(0,1) = 0;
		v(1,1) = buf_fy * z;
		v(2,1) = -buf_fy * py * z_sqr;
		v(3,1) = -buf_fy - buf_fy * py * py * z_sqr;
		v(4,1) = buf_fy * px * py * z_sqr;
		v(5,1) = buf_fy * px * z;

		res[0] = rx;
		res[1] = ry;


		// step 6: integrate into A and b:
		ls.update(v, res, *(project_buf_weight_p+i));
	}

	// cout << "pro_A: " << endl;
	// cout << ls.pro_A << endl;

	Vector6 result;

	// solve ls
	ls.finish();
	ls.solve_union(result);

	return result;
}


//# 
float SE3Tracker::ProjectionResiduals(
		TrackingReference* reference,
		Frame* frame,
		const Sophus::SE3f& referenceToFrame,
		int level)
{

	bool mbCheckOrientation = false;

	// 长宽和相机内参
	int w = frame->width(level);
	int h = frame->height(level);
	int w0 = frame->width(0);
	Eigen::Matrix3f KLvl = frame->K(0);
	float fx = KLvl(0,0);
	float fy = KLvl(1,1);
	float cx = KLvl(0,2);
	float cy = KLvl(1,2);

	float fxInvLevel = reference->keyframe->fxInv(0);
	float fyInvLevel = reference->keyframe->fyInv(0);
	float cxInvLevel = reference->keyframe->cxInv(0);
	float cyInvLevel = reference->keyframe->cyInv(0);

	// 位姿变换旋转矩阵和平移矩阵
	Eigen::Matrix3f rotMat = referenceToFrame.rotationMatrix();
	Eigen::Vector3f transVec = referenceToFrame.translation();

	const float* pyrIdepthSource = reference->keyframe->idepth(0);
	const float* pyrIdepthVarSource = reference->keyframe->idepthVar(0);
	const float* pyrColorSource = reference->keyframe->image(0);
	const Eigen::Vector4f* pyrGradSource = reference->keyframe->gradients(0);

	if(reference->projectposData[level] == nullptr) reference->projectposData[level] = new Eigen::Vector3f[w*h];
	if(reference->projectgradData[level] == nullptr) reference->projectgradData[level] = new Eigen::Vector2f[w*h];
	if(reference->projectcolorAndVarData[level] == nullptr) reference->projectcolorAndVarData[level] = new Eigen::Vector2f[w*h];

	Eigen::Vector3f* posDataPT = reference->projectposData[level];
	Eigen::Vector2f* gradDataPT = reference->projectgradData[level];
	Eigen::Vector2f* colorAndVarDataPT = reference->projectcolorAndVarData[level];

	const Eigen::Vector4f* frame_gradients = frame->gradients(0);

	// Rotation Histogram (to check rotation consistency)
    vector<int> rotHist[HISTO_LENGTH];
	const float factor = 1.0f/HISTO_LENGTH;
	
	//==================== 逐参考帧的关键点匹配 ==========================
	int id_iterator = 0;
	int idxpt = 0;
	vector<int> vMatchedDistance(frame->fKeypoints.size(), 256);
	vector<int> vnMatches21(frame->fKeypoints.size(),-1);
	vector<int> vnMatches12(reference->keyframe->fKeypoints.size(),-1);
	//cout << "特征点数：" << reference->keyframe->fKeypoints.size() << endl;
	for (auto keypoint : reference->keyframe->fKeypoints)
	{
		if (keypoint.octave != level) //只匹配在这一层上的特征点
			continue;


		int x = round(keypoint.pt.x);//每一层的特征点都映射到第零层
		int y = round(keypoint.pt.y);
		int idx = x + y*w0;

		// 为了保障数量足够的特征点能被投影到当前帧,!!!!!!!!!!这里可能不能使用LSD的点云，要通过orb自己建图
		// if(pyrIdepthVarSource[idx] <= 0 || pyrIdepthSource[idx] == 0)
		// {
		// 	if(x<w-1) idx=x+1+y*w;
		// 	if(pyrIdepthVarSource[idx] <= 0 || pyrIdepthSource[idx] == 0)
		// 	{
		// 		if(x>1) idx=x-1+y*w;
		// 		if(pyrIdepthVarSource[idx] <= 0 || pyrIdepthSource[idx] == 0)
		// 		{
		// 			if(y<w-1) idx=x+(y+1)*w;
		// 			if(pyrIdepthVarSource[idx] <= 0 || pyrIdepthSource[idx] == 0)
		// 			{
		// 				if(y>1) idx=x+(y-1)*w;
		// 				if(pyrIdepthVarSource[idx] <= 0 || pyrIdepthSource[idx] == 0) continue;
		// 			}
		// 		}
		// 	}
		// }

		if(pyrIdepthVarSource[idx] <= 0 || pyrIdepthSource[idx] == 0)
		{
			//cout<<"参考帧关键点位置没有对应的3D点！！！！！"<<endl;
			posDataPT++;
			gradDataPT++;
			colorAndVarDataPT++;
			id_iterator++;
			continue;
		}

		// 构造3D点
		*posDataPT = (1.0f / pyrIdepthSource[idx]) * Eigen::Vector3f(fxInvLevel*x+cxInvLevel,fyInvLevel*y+cyInvLevel,1);
		*gradDataPT = pyrGradSource[idx].head<2>();
		*colorAndVarDataPT = Eigen::Vector2f(pyrColorSource[idx], pyrIdepthVarSource[idx]);

		//3D点投影到当前帧
		Eigen::Vector3f Wxp = rotMat * (*posDataPT) + transVec;
		float u_new = (Wxp[0]/Wxp[2])*fx + cx;
		float v_new = (Wxp[1]/Wxp[2])*fy + cy;

		if(!(u_new > 1 && v_new > 1 && u_new < w-2 && v_new < h-2)) 
		{
			posDataPT++;
			gradDataPT++;
			colorAndVarDataPT++;
			id_iterator++;
			continue;
		}

		Eigen::Vector3f resInterp = getInterpolatedElement43(frame_gradients, u_new, v_new, w);

		int nLastOctave = keypoint.octave;


		//====================== 选出当前帧的候选关键点 ==========================
		float radius = 15*reference->keyframe->mvScaleFactor[nLastOctave];
		vector<size_t> vIndices;//待匹配候选特征点
		vIndices = frame->GetFeaturesInArea(u_new,v_new, radius, nLastOctave-1, nLastOctave+1);//只搜索同一层的特征点

		if(vIndices.empty()) 
		{
			posDataPT++;
			gradDataPT++;
			colorAndVarDataPT++;
			id_iterator++;
			continue;
		}

		//====================== 在候选关键点里选出匹配点 ========================
		const cv::Mat dMP = reference->keyframe->fDescriptors.row(id_iterator);

		int bestDist = 256;
		int bestDist2 = 256;
		int bestIdx = -1;


		for(vector<size_t>::const_iterator vit=vIndices.begin(), vend=vIndices.end(); vit!=vend; vit++)
		{
			const size_t i2 = *vit;

			const cv::Mat &d = frame->fDescriptors.row(i2);

			const int dist = DescriptorDistance(dMP,d);

			if(vMatchedDistance[i2]<=dist)
                continue;

            if(dist<bestDist)
            {
                bestDist2=bestDist;
                bestDist=dist;
                bestIdx=i2;
            }
            else if(dist<bestDist2)
            {
                bestDist2=dist;
            }
		}

		if(bestDist<=100)//100
		{
			if(bestDist<(float)bestDist2*0.9)//最好的匹配距离应该比次好的匹配距离的70%更小
			{
				if(vnMatches21[bestIdx]>=0)
                {
                    vnMatches12[vnMatches21[bestIdx]]=-1;
                    //reference->keyframe->nmatches--;
				}
				vnMatches12[id_iterator]=bestIdx;
                vnMatches21[bestIdx]=id_iterator;
                vMatchedDistance[bestIdx]=bestDist;
                reference->keyframe->nmatches++;

				if(mbCheckOrientation)
				{
					float rot = keypoint.angle-frame->fKeypoints[bestIdx].angle;
					if(rot<0.0)
						rot+=360.0f;
					int bin = round(rot*factor);
					if(bin==HISTO_LENGTH)
						bin=0;
					assert(bin>=0 && bin<HISTO_LENGTH);
					rotHist[bin].push_back(bestIdx);
				}
			}
		
			// 重投影误差
			*(project_buf_warped_residual_x+idxpt) = u_new - frame->fKeypoints[bestIdx].pt.x;
			*(project_buf_warped_residual_y+idxpt) = v_new - frame->fKeypoints[bestIdx].pt.y;

			*(project_buf_warped_x+idxpt) = Wxp(0);
			*(project_buf_warped_y+idxpt) = Wxp(1);
			*(project_buf_warped_z+idxpt) = Wxp(2);

			*(buf_warped_dx+idxpt) = fx * resInterp[0];
			*(buf_warped_dy+idxpt) = fy * resInterp[1];

			*(project_buf_d+idxpt) = 1.0f / (*posDataPT)[2];
			*(project_buf_idepthVar+idxpt) = (*colorAndVarDataPT)[1];
			idxpt++;

			if(*(project_buf_idepthVar+idxpt)<minidepthVar) minidepthVar = *(project_buf_idepthVar+idxpt);//找到最小逆深度方差
		}

		posDataPT++;
		gradDataPT++;
		colorAndVarDataPT++;
		id_iterator++;

	}
	project_buf_warped_size = idxpt;

	//cout<<"成功匹配的比率："<<(float)reference->keyframe->nmatches/(float)reference->keyframe->fKeypoints.size()<<endl;
	cout <<"第"<<level<< "层成功匹配个数：" << reference->keyframe->nmatches << endl;

	//Apply rotation consistency
	if(mbCheckOrientation)
	{
		int ind1=-1;
		int ind2=-1;
		int ind3=-1;

		ComputeThreeMaxima(rotHist,HISTO_LENGTH,ind1,ind2,ind3);

		for(int i=0; i<HISTO_LENGTH; i++)
		{
			if(i!=ind1 && i!=ind2 && i!=ind3)
			{
				for(size_t j=0, jend=rotHist[i].size(); j<jend; j++)
				{
					reference->keyframe->nmatches--;
				}
			}
		}
	}
	nmatches[level] = reference->keyframe->nmatches;

	reference->keyframe->nmatches=0;

	return 0;
}

int SE3Tracker::DescriptorDistance(const cv::Mat &a, const cv::Mat &b)
{
    const int *pa = a.ptr<int32_t>();
    const int *pb = b.ptr<int32_t>();

    int dist=0;

    for(int i=0; i<8; i++, pa++, pb++)
    {
        unsigned  int v = *pa ^ *pb;
        v = v - ((v >> 1) & 0x55555555);
        v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
        dist += (((v + (v >> 4)) & 0xF0F0F0F) * 0x1010101) >> 24;
    }

    return dist;
}

void SE3Tracker::ComputeThreeMaxima(vector<int>* histo, const int L, int &ind1, int &ind2, int &ind3)
{
    int max1=0;
    int max2=0;
    int max3=0;

    for(int i=0; i<L; i++)
    {
        const int s = histo[i].size();
        if(s>max1)
        {
            max3=max2;
            max2=max1;
            max1=s;
            ind3=ind2;
            ind2=ind1;
            ind1=i;
        }
        else if(s>max2)
        {
            max3=max2;
            max2=s;
            ind3=ind2;
            ind2=i;
        }
        else if(s>max3)
        {
            max3=s;
            ind3=i;
        }
    }

    if(max2<0.1f*(float)max1)
    {
        ind2=-1;
        ind3=-1;
    }
    else if(max3<0.1f*(float)max1)
    {
        ind3=-1;
    }
}


}

