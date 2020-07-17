#include <openvr_driver.h>

#include "bone_provider.h"
#include <fstream>
#include <string>
#include <windows.h>


static TCHAR calibrationMemName[] = TEXT("BoneCalibrationMemmap");

static HANDLE calibrationMemHandle = INVALID_HANDLE_VALUE;
static calibration_data_t* calibrationMem = NULL;

static void k4a_log_cb(void* context,
	k4a_log_level_t level,
	const char* file,
	const int line,
	const char* message)
{
	((K4ABoneProvider*)context)->m_driver_log("%d : %s:%d - %s\n", level, file, line, message);
}

K4ABoneProvider::K4ABoneProvider(DriverLog_t driver_log)
{
	m_driver_log = driver_log;

	calibrationMemHandle = CreateFileMapping(
		INVALID_HANDLE_VALUE,
		NULL,
		PAGE_READWRITE,
		0,
		CALIBRATION_MEMSIZE,
		calibrationMemName
	);

	if (calibrationMemHandle == nullptr)
		driver_log("Could not open file mapping object %d\n", GetLastError());
	else
	{
		calibrationMem = reinterpret_cast<calibration_data_t*>((LPTSTR)MapViewOfFile(calibrationMemHandle,
			FILE_MAP_ALL_ACCESS,
			0,
			0,
			CALIBRATION_MEMSIZE));

		if (calibrationMem == nullptr)
		{
			driver_log("Could not map view of file %d\n", GetLastError());

			CloseHandle(calibrationMemHandle);
		}
		else
		{
			calibrationMem->update = false;

			calibrationMem->x = 0.F;
			calibrationMem->y = 0.F;
			calibrationMem->z = 0.F;

			calibrationMem->rotOffset.w = 1.F;
			calibrationMem->rotOffset.x = 0.F;
			calibrationMem->rotOffset.y = 0.F;
			calibrationMem->rotOffset.z = 0.F;

			k4a_set_debug_message_handler(k4a_log_cb, this, k4a_log_level_t::K4A_LOG_LEVEL_ERROR);

			if (k4a_device_open(K4A_DEVICE_DEFAULT, &m_device) != K4A_RESULT_SUCCEEDED)
			{
				m_driver_log("Open K4A device failed\n");
				m_error = BONE_PROVIDER_OPEN_ERROR;
			}
			else
				m_open = true;
		}
	}
}

K4ABoneProvider::~K4ABoneProvider()
{
	k4a_device_close(m_device);
}

K4ABoneProviderError K4ABoneProvider::Configure(k4a_depth_mode_t new_depth_mode, float new_smoothing_rate)
{
	m_device_config.depth_mode = new_depth_mode;
	m_smoothing_rate = new_smoothing_rate;

	if (m_open)
	{
		if (K4A_RESULT_SUCCEEDED != k4a_device_get_calibration(m_device, m_device_config.depth_mode, K4A_COLOR_RESOLUTION_OFF, &m_calibration))
		{
			m_driver_log("Get depth camera calibration failed!\n");
			return BONE_PROVIDER_CALIB_ERROR;
		}
	}

	return BONE_PROVIDER_OPEN_ERROR;
}

K4ABoneProviderError K4ABoneProvider::Start()
{
	if (m_open)
	{

		if (k4a_device_start_cameras(m_device, &m_device_config) != K4A_RESULT_SUCCEEDED)
		{
			m_driver_log("Start camera failed\n");
			m_error = BONE_PROVIDER_CAMERA_START_ERROR;
			return m_error;
		}

		m_bone_thread = new std::thread(ProcessBones, this);
	}
	return BONE_PROVIDER_OPEN_ERROR;
}

K4ABoneProviderError K4ABoneProvider::Stop()
{
	if (m_open && m_online)
	{
		m_online = false;

		m_bone_thread->join();

		m_hip_pose.deviceIsConnected = false;
		m_rleg_pose.deviceIsConnected = false;
		m_lleg_pose.deviceIsConnected = false;
		m_relbow_pose.deviceIsConnected = false;
		m_lelbow_pose.deviceIsConnected = false;
		m_rknee_pose.deviceIsConnected = false;
		m_lknee_pose.deviceIsConnected = false;
		m_chest_pose.deviceIsConnected = false;

		m_hip_pose.poseIsValid = false;
		m_rleg_pose.poseIsValid = false;
		m_lleg_pose.poseIsValid = false;
		m_relbow_pose.poseIsValid = false;
		m_lelbow_pose.poseIsValid = false;
		m_rknee_pose.poseIsValid = false;
		m_lknee_pose.poseIsValid = false;
		m_chest_pose.poseIsValid = false;

		vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_hip_id, m_hip_pose, sizeof(vr::DriverPose_t));
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_rleg_id, m_rleg_pose, sizeof(vr::DriverPose_t));
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_lleg_id, m_lleg_pose, sizeof(vr::DriverPose_t));
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_relbow_id, m_relbow_pose, sizeof(vr::DriverPose_t));
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_lelbow_id, m_lelbow_pose, sizeof(vr::DriverPose_t));
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_rknee_id, m_rknee_pose, sizeof(vr::DriverPose_t));
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_lknee_id, m_lknee_pose, sizeof(vr::DriverPose_t));
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_chest_id, m_chest_pose, sizeof(vr::DriverPose_t));

		k4a_device_stop_cameras(m_device);

		m_error = BONE_PROVIDER_NO_ERROR;

		m_driver_log("Device is now offline for reconfiguration\n");
	}
	else
	{
		m_driver_log("Device is already offline\n");
	}

	return m_error;
}

void UpdateCalibration(vr::DriverPose_t& waist, vr::DriverPose_t& rightFoot, vr::DriverPose_t& leftFoot, vr::DriverPose_t& rightElbow, vr::DriverPose_t& leftElbow, vr::DriverPose_t& rightKnee, vr::DriverPose_t& leftKnee, vr::DriverPose_t& chest)
{
	// TODO:
	// if default values are changed, save changes to file
	// then
	// push file values onto variables

	float x = calibrationMem->x;
	float y = calibrationMem->y;
	float z = calibrationMem->z;

	float qw = calibrationMem->rotOffset.w;
	float qx = calibrationMem->rotOffset.x;
	float qy = calibrationMem->rotOffset.y;
	float qz = calibrationMem->rotOffset.z;

	waist.vecWorldFromDriverTranslation[0] = x;
	waist.vecWorldFromDriverTranslation[1] = y;
	waist.vecWorldFromDriverTranslation[2] = z;

	rightFoot.vecWorldFromDriverTranslation[0] = x;
	rightFoot.vecWorldFromDriverTranslation[1] = y;
	rightFoot.vecWorldFromDriverTranslation[2] = z;

	leftFoot.vecWorldFromDriverTranslation[0] = x;
	leftFoot.vecWorldFromDriverTranslation[1] = y;
	leftFoot.vecWorldFromDriverTranslation[2] = z;

	rightElbow.vecWorldFromDriverTranslation[0] = x;
	rightElbow.vecWorldFromDriverTranslation[1] = y;
	rightElbow.vecWorldFromDriverTranslation[2] = z;

	leftElbow.vecWorldFromDriverTranslation[0] = x;
	leftElbow.vecWorldFromDriverTranslation[1] = y;
	leftElbow.vecWorldFromDriverTranslation[2] = z;

	rightKnee.vecWorldFromDriverTranslation[0] = x;
	rightKnee.vecWorldFromDriverTranslation[1] = y;
	rightKnee.vecWorldFromDriverTranslation[2] = z;

	leftKnee.vecWorldFromDriverTranslation[0] = x;
	leftKnee.vecWorldFromDriverTranslation[1] = y;
	leftKnee.vecWorldFromDriverTranslation[2] = z;

	chest.vecWorldFromDriverTranslation[0] = x;
	chest.vecWorldFromDriverTranslation[1] = y;
	chest.vecWorldFromDriverTranslation[2] = z;


	waist.qWorldFromDriverRotation.w = qw;
	waist.qWorldFromDriverRotation.x = qx;
	waist.qWorldFromDriverRotation.y = qy;
	waist.qWorldFromDriverRotation.z = qz;

	rightFoot.qWorldFromDriverRotation.w = qw;
	rightFoot.qWorldFromDriverRotation.x = qx;
	rightFoot.qWorldFromDriverRotation.y = qy;
	rightFoot.qWorldFromDriverRotation.z = qz;

	leftFoot.qWorldFromDriverRotation.w = qw;
	leftFoot.qWorldFromDriverRotation.x = qx;
	leftFoot.qWorldFromDriverRotation.y = qy;
	leftFoot.qWorldFromDriverRotation.z = qz;

	rightElbow.qWorldFromDriverRotation.w = qw;
	rightElbow.qWorldFromDriverRotation.x = qx;
	rightElbow.qWorldFromDriverRotation.y = qy;
	rightElbow.qWorldFromDriverRotation.z = qz;

	leftElbow.qWorldFromDriverRotation.w = qw;
	leftElbow.qWorldFromDriverRotation.x = qx;
	leftElbow.qWorldFromDriverRotation.y = qy;
	leftElbow.qWorldFromDriverRotation.z = qz;

	rightKnee.qWorldFromDriverRotation.w = qw;
	rightKnee.qWorldFromDriverRotation.x = qx;
	rightKnee.qWorldFromDriverRotation.y = qy;
	rightKnee.qWorldFromDriverRotation.z = qz;

	leftKnee.qWorldFromDriverRotation.w = qw;
	leftKnee.qWorldFromDriverRotation.x = qx;
	leftKnee.qWorldFromDriverRotation.y = qy;
	leftKnee.qWorldFromDriverRotation.z = qz;

	
	chest.qWorldFromDriverRotation.w = qw;
	chest.qWorldFromDriverRotation.x = qx;
	chest.qWorldFromDriverRotation.y = qy;
	chest.qWorldFromDriverRotation.z = qz;
	
	

	calibrationMem->update = false;
}

void K4ABoneProvider::ProcessBones(K4ABoneProvider* context)
{
	// Wait for every bone to be activated before attempting to populate pose data
	while (context->m_hip_id == vr::k_unTrackedDeviceIndexInvalid || context->m_rleg_id == vr::k_unTrackedDeviceIndexInvalid || context->m_lleg_id == vr::k_unTrackedDeviceIndexInvalid || context->m_relbow_id == vr::k_unTrackedDeviceIndexInvalid || context->m_lelbow_id == vr::k_unTrackedDeviceIndexInvalid 
		|| context->m_rknee_id == vr::k_unTrackedDeviceIndexInvalid || context->m_lknee_id == vr::k_unTrackedDeviceIndexInvalid || context->m_chest_id == vr::k_unTrackedDeviceIndexInvalid)
		std::this_thread::sleep_for(std::chrono::milliseconds(33));

	// Bring thread relevent data into the thread stack

	k4a_device_t device = context->m_device;
	k4abt_tracker_t tracker;

	uint32_t hip_id = context->m_hip_id;
	uint32_t rleg_id = context->m_rleg_id;
	uint32_t lleg_id = context->m_lleg_id;
	uint32_t chest_id = context->m_chest_id;
	uint32_t relbow_id = context->m_relbow_id;
	uint32_t lelbow_id = context->m_lelbow_id;
	uint32_t rknee_id = context->m_rknee_id;
	uint32_t lknee_id = context->m_lknee_id;

	k4a_capture_t capture = nullptr;
	k4abt_frame_t body_frame = nullptr;

	if (k4abt_tracker_create(&context->m_calibration, ::K4ABT_TRACKER_CONFIG_DEFAULT, &tracker) == K4A_RESULT_SUCCEEDED)
	{
		k4abt_tracker_set_temporal_smoothing(tracker, 0.025F);

		if (!context->IsOnline())
		{
			context->m_online = true;

			context->m_hip_pose.deviceIsConnected = true;
			context->m_rleg_pose.deviceIsConnected = true;
			context->m_lleg_pose.deviceIsConnected = true;
			context->m_chest_pose.deviceIsConnected = true;
			context->m_relbow_pose.deviceIsConnected = true;
			context->m_lelbow_pose.deviceIsConnected = true;
			context->m_rknee_pose.deviceIsConnected = true;
			context->m_lknee_pose.deviceIsConnected = true;

			vr::VRServerDriverHost()->TrackedDevicePoseUpdated(hip_id, context->m_hip_pose, sizeof(vr::DriverPose_t));
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated(rleg_id, context->m_rleg_pose, sizeof(vr::DriverPose_t));
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated(lleg_id, context->m_lleg_pose, sizeof(vr::DriverPose_t));
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated(hip_id, context->m_chest_pose, sizeof(vr::DriverPose_t));
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated(rleg_id, context->m_relbow_pose, sizeof(vr::DriverPose_t));
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated(lleg_id, context->m_lelbow_pose, sizeof(vr::DriverPose_t));
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated(rleg_id, context->m_rknee_pose, sizeof(vr::DriverPose_t));
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated(lleg_id, context->m_lknee_pose, sizeof(vr::DriverPose_t));
		}

		uint64_t last_timestamp = 0;

		// recreate stack copies from the calibrated baseline pose
		vr::DriverPose_t hip_pose = context->m_hip_pose;
		vr::DriverPose_t rleg_pose = context->m_rleg_pose;
		vr::DriverPose_t lleg_pose = context->m_lleg_pose;
		vr::DriverPose_t chest_pose = context->m_chest_pose;
		vr::DriverPose_t relbow_pose = context->m_relbow_pose;
		vr::DriverPose_t lelbow_pose = context->m_lelbow_pose;
		vr::DriverPose_t rknee_pose = context->m_rknee_pose;
		vr::DriverPose_t lknee_pose = context->m_lknee_pose;
		double rotationChange;
		double vectorChange = 0.0;
		double maxChange = 1.0;
		float smoothing = 0.0f;
		bool firstRun = true;

		while (context->m_online)
		{
			time_t lastTime = time(NULL);
			if (k4a_device_get_capture(device, &capture, K4A_WAIT_INFINITE) != K4A_WAIT_RESULT_SUCCEEDED)
				continue;
			else
			{
				if (k4abt_tracker_enqueue_capture(tracker, capture, 16) != K4A_WAIT_RESULT_SUCCEEDED)
				{
					k4a_capture_release(capture);
					continue;
				}
				else
				{
					if (k4abt_tracker_pop_result(tracker, &body_frame, 16) != K4A_WAIT_RESULT_SUCCEEDED)
					{
						k4a_capture_release(capture);
						continue;
					}
					else
					{
						if (int num_bodies = k4abt_frame_get_num_bodies(body_frame) != 0)
						{
							if (firstRun) {
								calibrationMem->update = false;
								calibrationMem->autoSmooth = false;
								UpdateCalibration(hip_pose, rleg_pose, lleg_pose, relbow_pose, lelbow_pose, rknee_pose, lknee_pose, chest_pose);
								firstRun = false;
								time(&lastTime);
							}
							if (calibrationMem->update)
								UpdateCalibration(hip_pose, rleg_pose, lleg_pose, relbow_pose, lelbow_pose, rknee_pose, lknee_pose, chest_pose);
								if (!calibrationMem->autoSmooth)
									k4abt_tracker_set_temporal_smoothing(tracker, calibrationMem->m_smoothing);
							// ???TODO: put autoSmooth on a different thread for efficiency???
							if (calibrationMem->autoSmooth)
								k4abt_tracker_set_temporal_smoothing(tracker, (((vectorChange / maxChange) > calibrationMem->m_smoothing) ? (smoothing = 0) : smoothing += 0.45 * (1 - smoothing)));
								vectorChange = 0;
							for (int i = 0; i < num_bodies; i++)
							{
								k4abt_skeleton_t skeleton;
								if (k4abt_frame_get_body_skeleton(body_frame, i, &skeleton) != K4A_RESULT_SUCCEEDED)
								{
									hip_pose.poseIsValid = false;
									rleg_pose.poseIsValid = false;
									lleg_pose.poseIsValid = false;
									chest_pose.poseIsValid = false;
									relbow_pose.poseIsValid = false;
									lelbow_pose.poseIsValid = false;
									rknee_pose.poseIsValid = false;
									lknee_pose.poseIsValid = false;

									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(hip_id, hip_pose, sizeof(vr::DriverPose_t));
									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(rleg_id, rleg_pose, sizeof(vr::DriverPose_t));
									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(lleg_id, lleg_pose, sizeof(vr::DriverPose_t));
									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(chest_id, chest_pose, sizeof(vr::DriverPose_t));
									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(relbow_id, relbow_pose, sizeof(vr::DriverPose_t));
									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(lelbow_id, lelbow_pose, sizeof(vr::DriverPose_t));
									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(rknee_id, rknee_pose, sizeof(vr::DriverPose_t));
									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(lknee_id, lknee_pose, sizeof(vr::DriverPose_t));
								}
								else
								{
									double seconds = difftime(lastTime, time(NULL));
									time(&lastTime);
									float temp = 0;

									hip_pose.poseIsValid = true;
									hip_pose.qRotation.w = skeleton.joints[K4ABT_JOINT_PELVIS].orientation.wxyz.w;
									hip_pose.qRotation.x = skeleton.joints[K4ABT_JOINT_PELVIS].orientation.wxyz.z;
									hip_pose.qRotation.y = skeleton.joints[K4ABT_JOINT_PELVIS].orientation.wxyz.x;
									hip_pose.qRotation.z = skeleton.joints[K4ABT_JOINT_PELVIS].orientation.wxyz.y;
									hip_pose.vecPosition[0] = skeleton.joints[K4ABT_JOINT_PELVIS].position.xyz.z / 1000;
									hip_pose.vecPosition[1] = skeleton.joints[K4ABT_JOINT_PELVIS].position.xyz.x / 1000;
									hip_pose.vecPosition[2] = skeleton.joints[K4ABT_JOINT_PELVIS].position.xyz.y / 1000;

									rleg_pose.poseIsValid = true;
									rleg_pose.qRotation.w = skeleton.joints[K4ABT_JOINT_ANKLE_RIGHT].orientation.wxyz.w;
									rleg_pose.qRotation.x = skeleton.joints[K4ABT_JOINT_ANKLE_RIGHT].orientation.wxyz.z;
									rleg_pose.qRotation.y = skeleton.joints[K4ABT_JOINT_ANKLE_RIGHT].orientation.wxyz.x;
									rleg_pose.qRotation.z = skeleton.joints[K4ABT_JOINT_ANKLE_RIGHT].orientation.wxyz.y;
									//velocity calculated by newPos - oldPos / seconds between two points
									rleg_pose.vecVelocity[0] = ((temp = ((skeleton.joints[K4ABT_JOINT_FOOT_RIGHT].position.xyz.z + skeleton.joints[K4ABT_JOINT_ANKLE_RIGHT].position.xyz.z) / 2000)) - rleg_pose.vecPosition[0]) / 0.08;
									rleg_pose.vecPosition[0] = temp;
									rleg_pose.vecVelocity[1] = ((temp = ((skeleton.joints[K4ABT_JOINT_FOOT_RIGHT].position.xyz.x + skeleton.joints[K4ABT_JOINT_ANKLE_RIGHT].position.xyz.x) / 2000)) - rleg_pose.vecPosition[1]) / 0.08;
									rleg_pose.vecPosition[1] = temp;
									rleg_pose.vecVelocity[2] = ((temp = ((skeleton.joints[K4ABT_JOINT_FOOT_RIGHT].position.xyz.y + skeleton.joints[K4ABT_JOINT_ANKLE_RIGHT].position.xyz.y) /2000 )) - rleg_pose.vecPosition[2]) / 0.08;
									rleg_pose.vecPosition[2] = temp;

									//update vectorChange for autoSmooth
									vectorChange += rleg_pose.vecVelocity[0] + rleg_pose.vecVelocity[1] + rleg_pose.vecVelocity[2];

									lleg_pose.poseIsValid = true;
									lleg_pose.qRotation.w = skeleton.joints[K4ABT_JOINT_ANKLE_LEFT].orientation.wxyz.w;
									//lleg_pose.vecAngularVelocity[0] = ((temp = skeleton.joints[K4ABT_JOINT_ANKLE_LEFT].orientation.wxyz.z) - lleg_pose.vecAngularVelocity[0]) / 0.1;
									lleg_pose.qRotation.x = temp = skeleton.joints[K4ABT_JOINT_ANKLE_LEFT].orientation.wxyz.z;
									//lleg_pose.vecAngularVelocity[1] = ((temp = skeleton.joints[K4ABT_JOINT_ANKLE_LEFT].orientation.wxyz.x) - lleg_pose.vecAngularVelocity[1]) / 0.1;
									lleg_pose.qRotation.y = skeleton.joints[K4ABT_JOINT_ANKLE_LEFT].orientation.wxyz.x;
									//lleg_pose.vecAngularVelocity[2] = ((temp = skeleton.joints[K4ABT_JOINT_ANKLE_LEFT].orientation.wxyz.y) - lleg_pose.vecAngularVelocity[2]) / 0.1;
									lleg_pose.qRotation.z = skeleton.joints[K4ABT_JOINT_ANKLE_LEFT].orientation.wxyz.y;

									lleg_pose.vecVelocity[0] = ((temp = ((skeleton.joints[K4ABT_JOINT_FOOT_LEFT].position.xyz.z + skeleton.joints[K4ABT_JOINT_ANKLE_LEFT].position.xyz.z) / 2000)) - lleg_pose.vecPosition[0]) / 0.08;
									lleg_pose.vecPosition[0] = temp;
									lleg_pose.vecVelocity[1] = ((temp = ((skeleton.joints[K4ABT_JOINT_FOOT_LEFT].position.xyz.x + skeleton.joints[K4ABT_JOINT_ANKLE_LEFT].position.xyz.x) / 2000)) - lleg_pose.vecPosition[1]) / 0.08;
									lleg_pose.vecPosition[1] = temp;
									lleg_pose.vecVelocity[2] = ((temp = ((skeleton.joints[K4ABT_JOINT_FOOT_LEFT].position.xyz.y + skeleton.joints[K4ABT_JOINT_ANKLE_LEFT].position.xyz.y) / 2000)) - lleg_pose.vecPosition[2]) / 0.08;
									lleg_pose.vecPosition[2] = temp;
									
									vectorChange += lleg_pose.vecVelocity[0] + lleg_pose.vecVelocity[1] + lleg_pose.vecVelocity[2];

									if (calibrationMem->moreTrackers) {
										
										chest_pose.poseIsValid = true;
										chest_pose.qRotation.w = skeleton.joints[K4ABT_JOINT_SPINE_CHEST].orientation.wxyz.w;
										chest_pose.qRotation.x = skeleton.joints[K4ABT_JOINT_SPINE_CHEST].orientation.wxyz.z;
										chest_pose.qRotation.y = skeleton.joints[K4ABT_JOINT_SPINE_CHEST].orientation.wxyz.x;
										chest_pose.qRotation.z = skeleton.joints[K4ABT_JOINT_SPINE_CHEST].orientation.wxyz.y;
										chest_pose.vecPosition[0] = skeleton.joints[K4ABT_JOINT_SPINE_CHEST].position.xyz.z / 1000;
										chest_pose.vecPosition[1] = skeleton.joints[K4ABT_JOINT_SPINE_CHEST].position.xyz.x / 1000;
										chest_pose.vecPosition[2] = skeleton.joints[K4ABT_JOINT_SPINE_CHEST].position.xyz.y / 1000;


										//elbow velocities are scaled to try and fix the inaccurate positional data
										relbow_pose.poseIsValid = true;
										relbow_pose.qRotation.w = skeleton.joints[K4ABT_JOINT_ELBOW_RIGHT].orientation.wxyz.w;
										relbow_pose.qRotation.x = skeleton.joints[K4ABT_JOINT_ELBOW_RIGHT].orientation.wxyz.z;
										relbow_pose.qRotation.y = skeleton.joints[K4ABT_JOINT_ELBOW_RIGHT].orientation.wxyz.x;
										relbow_pose.qRotation.z = skeleton.joints[K4ABT_JOINT_ELBOW_RIGHT].orientation.wxyz.y;
										relbow_pose.vecVelocity[0] = 1.2 * ((temp = ((skeleton.joints[K4ABT_JOINT_ELBOW_RIGHT].position.xyz.z) / 1000)) - relbow_pose.vecPosition[0]) / 0.08;
										relbow_pose.vecPosition[0] = temp;
										relbow_pose.vecVelocity[1] = 1.2 * ((temp = ((skeleton.joints[K4ABT_JOINT_ELBOW_RIGHT].position.xyz.x) / 1000)) - relbow_pose.vecPosition[1]) / 0.08;
										relbow_pose.vecPosition[1] = temp;
										relbow_pose.vecVelocity[2] = 1.2 * ((temp = ((skeleton.joints[K4ABT_JOINT_ELBOW_RIGHT].position.xyz.y) / 1000)) - relbow_pose.vecPosition[2]) / 0.08;
										relbow_pose.vecPosition[2] = temp;

										lelbow_pose.poseIsValid = true;
										lelbow_pose.qRotation.w = skeleton.joints[K4ABT_JOINT_ELBOW_LEFT].orientation.wxyz.w;
										lelbow_pose.qRotation.x = skeleton.joints[K4ABT_JOINT_ELBOW_LEFT].orientation.wxyz.z;
										lelbow_pose.qRotation.y = skeleton.joints[K4ABT_JOINT_ELBOW_LEFT].orientation.wxyz.x;
										lelbow_pose.qRotation.z = skeleton.joints[K4ABT_JOINT_ELBOW_LEFT].orientation.wxyz.y;
										lelbow_pose.vecVelocity[0] = 1.2 * ((temp = ((skeleton.joints[K4ABT_JOINT_ELBOW_LEFT].position.xyz.z) / 1000)) - lelbow_pose.vecPosition[0]) / 0.08;
										lelbow_pose.vecPosition[0] = temp;
										lelbow_pose.vecVelocity[1] = 1.2 * ((temp = ((skeleton.joints[K4ABT_JOINT_ELBOW_LEFT].position.xyz.x) / 1000)) - lelbow_pose.vecPosition[1]) / 0.08;
										lelbow_pose.vecPosition[1] = temp;
										lelbow_pose.vecVelocity[2] = 1.2 * ((temp = ((skeleton.joints[K4ABT_JOINT_ELBOW_LEFT].position.xyz.y) / 1000)) - lelbow_pose.vecPosition[2]) / 0.08;
										lelbow_pose.vecPosition[2] = temp;

										

										lknee_pose.poseIsValid = true;
										lknee_pose.qRotation.w = skeleton.joints[K4ABT_JOINT_KNEE_LEFT].orientation.wxyz.w;
										lknee_pose.qRotation.x = skeleton.joints[K4ABT_JOINT_KNEE_LEFT].orientation.wxyz.z;
										lknee_pose.qRotation.y = skeleton.joints[K4ABT_JOINT_KNEE_LEFT].orientation.wxyz.x;
										lknee_pose.qRotation.z = skeleton.joints[K4ABT_JOINT_KNEE_LEFT].orientation.wxyz.y;
										lknee_pose.vecVelocity[0] = ((temp = ((skeleton.joints[K4ABT_JOINT_KNEE_LEFT].position.xyz.z) / 1000)) - lknee_pose.vecPosition[0]) / 0.08;
										lknee_pose.vecPosition[0] = temp;
										lknee_pose.vecVelocity[1] = ((temp = ((skeleton.joints[K4ABT_JOINT_KNEE_LEFT].position.xyz.x) / 1000)) - lknee_pose.vecPosition[1]) / 0.08;
										lknee_pose.vecPosition[1] = temp;
										lknee_pose.vecVelocity[2] = ((temp = ((skeleton.joints[K4ABT_JOINT_KNEE_LEFT].position.xyz.y) / 1000)) - lknee_pose.vecPosition[2]) / 0.08;
										lknee_pose.vecPosition[2] = temp;

										rknee_pose.poseIsValid = true;
										rknee_pose.qRotation.w = skeleton.joints[K4ABT_JOINT_KNEE_RIGHT].orientation.wxyz.w;
										rknee_pose.qRotation.x = skeleton.joints[K4ABT_JOINT_KNEE_RIGHT].orientation.wxyz.z;
										rknee_pose.qRotation.y = skeleton.joints[K4ABT_JOINT_KNEE_RIGHT].orientation.wxyz.x;
										rknee_pose.qRotation.z = skeleton.joints[K4ABT_JOINT_KNEE_RIGHT].orientation.wxyz.y;
										rknee_pose.vecVelocity[0] = ((temp = ((skeleton.joints[K4ABT_JOINT_KNEE_RIGHT].position.xyz.z) / 1000)) - rknee_pose.vecPosition[0]) / 0.08;
										rknee_pose.vecPosition[0] = temp;
										rknee_pose.vecVelocity[1] = ((temp = ((skeleton.joints[K4ABT_JOINT_KNEE_RIGHT].position.xyz.x) / 1000)) - rknee_pose.vecPosition[1]) / 0.08;
										rknee_pose.vecPosition[1] = temp;
										rknee_pose.vecVelocity[2] = ((temp = ((skeleton.joints[K4ABT_JOINT_KNEE_RIGHT].position.xyz.y) / 1000)) - rknee_pose.vecPosition[2]) / 0.08;
										rknee_pose.vecPosition[2] = temp;
									}
									

									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(hip_id, hip_pose, sizeof(vr::DriverPose_t));
									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(rleg_id, rleg_pose, sizeof(vr::DriverPose_t));
									vr::VRServerDriverHost()->TrackedDevicePoseUpdated(lleg_id, lleg_pose, sizeof(vr::DriverPose_t));
									if (calibrationMem->moreTrackers) {
										vr::VRServerDriverHost()->TrackedDevicePoseUpdated(chest_id, chest_pose, sizeof(vr::DriverPose_t));
										vr::VRServerDriverHost()->TrackedDevicePoseUpdated(relbow_id, relbow_pose, sizeof(vr::DriverPose_t));
										vr::VRServerDriverHost()->TrackedDevicePoseUpdated(lelbow_id, lelbow_pose, sizeof(vr::DriverPose_t));
										vr::VRServerDriverHost()->TrackedDevicePoseUpdated(rknee_id, rknee_pose, sizeof(vr::DriverPose_t));
										vr::VRServerDriverHost()->TrackedDevicePoseUpdated(lknee_id, lknee_pose, sizeof(vr::DriverPose_t));
									}
									

									if(calibrationMem->autoSmooth)
										maxChange = max(vectorChange, (maxChange - (maxChange * 0.98)));
									
								}
							}
						}
						k4abt_frame_release(body_frame);
					}
				}
				k4a_capture_release(capture);
			}
		}

		k4abt_tracker_destroy(tracker);
	}
}

void K4ABoneProvider::setup_bone(uint32_t unObjectId, k4abt_joint_id_t bone)
{
	vr::DriverPose_t bone_pose = { 0 };

	bone_pose.poseTimeOffset = 0.11F;

	bone_pose.qDriverFromHeadRotation.w = 1.F;
	bone_pose.qDriverFromHeadRotation.x = 0.F;
	bone_pose.qDriverFromHeadRotation.y = 0.F;
	bone_pose.qDriverFromHeadRotation.z = 0.F;

	bone_pose.qWorldFromDriverRotation.w = std::cos(std::acos(-1) / 4);
	bone_pose.qWorldFromDriverRotation.x = std::sin(std::acos(-1) / 4);
	bone_pose.qWorldFromDriverRotation.y = 0.F;
	bone_pose.qWorldFromDriverRotation.z = 0.F;

	bone_pose.vecWorldFromDriverTranslation[0] = 0.F;
	bone_pose.vecWorldFromDriverTranslation[1] = 0.F;
	bone_pose.vecWorldFromDriverTranslation[2] = 0.F;

	bone_pose.vecDriverFromHeadTranslation[0] = 0.F;
	bone_pose.vecDriverFromHeadTranslation[1] = 0.F;
	bone_pose.vecDriverFromHeadTranslation[2] = 0.F;

	bone_pose.vecVelocity[0] = 0.F;
	bone_pose.vecVelocity[1] = 0.F;
	bone_pose.vecVelocity[2] = 0.F;

	bone_pose.vecAcceleration[0] = 0.F;
	bone_pose.vecAcceleration[1] = 0.F;
	bone_pose.vecAcceleration[2] = 0.F;

	bone_pose.vecAngularVelocity[0] = 0.0;
	bone_pose.vecAngularVelocity[1] = 0.0;
	bone_pose.vecAngularVelocity[2] = 0.0;

	bone_pose.vecAngularAcceleration[0] = 0.0;
	bone_pose.vecAngularAcceleration[1] = 0.0;
	bone_pose.vecAngularAcceleration[2] = 0.0;

	bone_pose.willDriftInYaw = false;
	bone_pose.shouldApplyHeadModel = false;

	if (bone == K4ABT_JOINT_PELVIS)
	{
		m_hip_pose = bone_pose;
		m_hip_id = unObjectId;
	}
	else if (bone == K4ABT_JOINT_FOOT_RIGHT)
	{
		m_rleg_pose = bone_pose;
		m_rleg_id = unObjectId;
	}
	else if (bone == K4ABT_JOINT_FOOT_LEFT)
	{
		m_lleg_pose = bone_pose;
		m_lleg_id = unObjectId;
	}
	else if (bone == K4ABT_JOINT_SPINE_CHEST)
	{
		m_chest_pose = bone_pose;
		m_chest_id = unObjectId;
	}
	else if (bone == K4ABT_JOINT_ELBOW_RIGHT)
	{
		m_relbow_pose = bone_pose;
		m_relbow_id = unObjectId;
	}
	else if (bone == K4ABT_JOINT_ELBOW_LEFT)
	{
		m_lelbow_pose = bone_pose;
		m_lelbow_id = unObjectId;
	}
	else if (bone == K4ABT_JOINT_KNEE_RIGHT)
	{
		m_rknee_pose = bone_pose;
		m_rknee_id = unObjectId;
	}
	else
	{
		m_lknee_pose = bone_pose;
		m_lknee_id = unObjectId;
	}

}



