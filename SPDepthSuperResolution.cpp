#include "SPDepthSuperResolution.h"
#include "SuperpixelSegmentation\DepthAdaptiveSuperpixel.h"
#include "EdgeRefinedSuperpixel\EdgeRefinedSuperpixel.h"
#include "DimensionConvertor\DimensionConvertor.h"
#include "NormalEstimation\NormalMapGenerator.h"
#include "LabelEquivalenceSeg\LabelEquivalenceSeg.h"
#include "Projection_GPU\Projection_GPU.h"
#include "Cluster\Cluster.h"

SPDepthSuperResolution::SPDepthSuperResolution(int width, int height):
	Width(width),
	Height(height){
		DASP = new DepthAdaptiveSuperpixel(width, height);
		SP = new DepthAdaptiveSuperpixel(width, height);
		ERS = new EdgeRefinedSuperpixel(width, height);
		Convertor = new DimensionConvertor();
		//spMerging = new LabelEquivalenceSeg(width, height);
		cudaMalloc(&EdgeEnhanced3DPoints_Device, width * height * sizeof(float3));
		cudaMallocHost(&EdgeEnhanced3DPoints_Host, width * height * sizeof(float3));
	}
SPDepthSuperResolution::~SPDepthSuperResolution(){
	delete DASP;
	DASP = 0;
	delete SP;
	SP = 0;
	delete ERS;
	ERS = 0;
	//delete spMerging;
	//spMerging = 0;
	delete Projector;
	Projector = 0;
	cudaFree(EdgeEnhanced3DPoints_Device);
	cudaFree(EdgeEnhanced3DPoints_Host);
	delete [] Cluster_Array;
	Cluster_Array = 0;
	cudaFree(ClusterND_Host);
	cudaFree(ClusterND_Device);
	cudaFree(ClusterCenter_Host);
	cudaFree(ClusterCenter_Device);
	
}
void SPDepthSuperResolution::SetParametor(int rows, int cols, cv::Mat_<double> intrinsic){
	sp_rows = rows;
	sp_cols = cols;
	SP->SetParametor(sp_rows, sp_cols, intrinsic);
	DASP->SetParametor(sp_rows, sp_cols, intrinsic);
	Convertor->setCameraParameters(intrinsic, Width, Height);
	Projector = new Projection_GPU(Width, Height, intrinsic);
	Cluster_Array = new Cluster[rows*cols];
	cudaMallocHost(&ClusterND_Host, sizeof(float4)*rows*cols);
	cudaMalloc(&ClusterND_Device, sizeof(float4)*rows*cols);
	cudaMallocHost(&ClusterCenter_Host, sizeof(float3)*rows*cols);
	cudaMalloc(&ClusterCenter_Device, sizeof(float3)*rows*cols);
	
}
void SPDepthSuperResolution::Process(float* depth_device, float3* points_device, cv::gpu::GpuMat color_device){
	//segmentation
	SP->Segmentation(color_device, points_device, 200.0f, 10.0f, 0.0f, 5);
	DASP->Segmentation(color_device, points_device, 0.0f, 10.0f, 200.0f, 5);
	//edge refinement
	ERS->EdgeRefining(SP->getLabelDevice(), DASP->getLabelDevice(), depth_device, color_device);
	//convert to realworld
	Convertor->projectiveToReal(ERS->getRefinedDepth_Device(), EdgeEnhanced3DPoints_Device);
	cudaMemcpy(EdgeEnhanced3DPoints_Host, EdgeEnhanced3DPoints_Device, sizeof(float3)*Width*Height, cudaMemcpyDeviceToHost);
	//clustering
	for(int y=0; y<Height; y++){
		for(int x=0; x<Width; x++){
			int label = ERS->getRefinedLabels_Host()[y*Width+x];
			if(label != -1){
				//store points into cluster
				cv::Mat point = (cv::Mat_<double>(1, 3) << (double)EdgeEnhanced3DPoints_Host[y*Width+x].x, 
																	(double)EdgeEnhanced3DPoints_Host[y*Width+x].y, 
																		(double)EdgeEnhanced3DPoints_Host[y*Width+x].z);
				//push back
				Cluster_Array[label].AddCluster3Dpoints(point);
			}
		}
	}
	//normal estimation using PCA
	//Cluster���ƂɃp�����[�^���v�Z
	for(int cluster_id=0; cluster_id <sp_rows*sp_cols; cluster_id++){	
		//�e�̈�̓_�Q�̐���臒l
		if(Cluster_Array[cluster_id].GetClusterSize() >= 3){
			//PCA
			cv::PCA PrincipalComponent(*(Cluster_Array[cluster_id].GetCluster3Dpoints()), cv::Mat(), CV_PCA_DATA_AS_ROW);
			//�@���x�N�g�������߂�
			float3 nor;
			nor.x = (float)PrincipalComponent.eigenvectors.at<double>(2,0);
			nor.y = (float)PrincipalComponent.eigenvectors.at<double>(2,1);
			nor.z = (float)PrincipalComponent.eigenvectors.at<double>(2,2);
			//�d�S�x�N�g��
			cv::Point3d g;
			g.x = PrincipalComponent.mean.at<double>(0, 0);
			g.y = PrincipalComponent.mean.at<double>(0, 1);
			g.z = PrincipalComponent.mean.at<double>(0, 2);
			//�d�S�܂ł̋���
			double g_distance = sqrt(g.x*g.x + g.y*g.y + g.z*g.z);
			Cluster_Array[cluster_id].SetClusterDistance(g_distance);
			ClusterCenter_Host[cluster_id].x = (float)g.x;
			ClusterCenter_Host[cluster_id].y = (float)g.y;
			ClusterCenter_Host[cluster_id].z = (float)g.z;
			Cluster_Array[cluster_id].SetClusterCenter(g);
			//���ʂƌ��_�̋���
			double plane_d_tmp = nor.x * g.x + nor.y * g.y + nor.z * g.z;
			//normal�����Ε������������ꍇ
			if(plane_d_tmp < 0){
				nor.x *= -1.0;
				nor.y *= -1.0;
				nor.z *= -1.0;
			}
			//normal�i�[
			Cluster_Array[cluster_id].SetNormal(nor);
			ClusterND_Host[cluster_id].x = nor.x;
			ClusterND_Host[cluster_id].y = nor.y;
			ClusterND_Host[cluster_id].z = nor.z;
			//���ʂ܂ł̋����擾
			double plane_d = fabs(plane_d_tmp);
			Cluster_Array[cluster_id].SetPlaneDistance(plane_d);
			ClusterND_Host[cluster_id].w = (float)plane_d;
			//eigenvalue���g���ĕ��ʂ����f
			double eigenvalues1 = PrincipalComponent.eigenvalues.at<double>(0, 0)/PrincipalComponent.eigenvalues.at<double>(1, 0);
			double eigenvalues2 = PrincipalComponent.eigenvalues.at<double>(2, 0);
			//���ʂɂł��Ȃ��Ƃ�
			if(!Cluster_Array[cluster_id].canPlane(eigenvalues1, eigenvalues2)){
				/*Plane_Num--;
				Cluster_ND[cluster_id].x = 5.0;
				Cluster_ND[cluster_id].y = 5.0;
				Cluster_ND[cluster_id].z = 5.0;*/
			}
		}
		//cluster�̂R�����_�����Ȃ�����Ƃ�
		else{
			Cluster_Array[cluster_id].canPlane(false);
			ClusterND_Host[cluster_id].x = 5.0;
			ClusterND_Host[cluster_id].y = 5.0;
			ClusterND_Host[cluster_id].z = 5.0;
		}
		//�}�b�v��clear
		Cluster_Array[cluster_id].ClearCluster3Dpoints();
	}	
	cudaMemcpy(ClusterND_Device, ClusterND_Host, sizeof(float4)*sp_rows*sp_cols, cudaMemcpyHostToDevice);
	cudaMemcpy(ClusterCenter_Device, ClusterCenter_Host, sizeof(float3)*sp_rows*sp_cols, cudaMemcpyHostToDevice);
	//superpixel merging
	//spMerging->labelImage(ClusterND_Device, ERS->getRefinedLabels_Device(), ClusterCenter_Device);
	//plane projection
	Projector->PlaneProjection(ClusterND_Device, ERS->getRefinedLabels_Device(), EdgeEnhanced3DPoints_Device);
}
float*	SPDepthSuperResolution::getRefinedDepth_Device(){
	return ERS->getRefinedDepth_Device();
}
float*	SPDepthSuperResolution::getRefinedDepth_Host(){
	return ERS->getRefinedDepth_Host();
}
float3*	SPDepthSuperResolution::getOptimizedPoints_Device(){
	return Projector->GetOptimized3D_Device();
}
float3*	SPDepthSuperResolution::getOptimizedPoints_Host(){
	return Projector->GetOptimized3D_Host();
}