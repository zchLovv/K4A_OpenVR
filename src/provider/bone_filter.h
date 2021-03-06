#include "k4abt.h"
#include "SimpleKalmanFilter.h"
#include <cmath>

#ifndef bone_filter_h
#define bone_filter_h

inline k4a_quaternion_t normalize(k4a_quaternion_t a) {
	float mag = pow(sqrt(a.wxyz.w * a.wxyz.w + a.wxyz.x * a.wxyz.x + a.wxyz.y * a.wxyz.y + a.wxyz.z * a.wxyz.z), 2);
	a.wxyz.w /= mag;
	a.wxyz.x /= mag;
	a.wxyz.y /= mag;
	a.wxyz.z /= mag;
	return a;
}

class bone_filter {

public:
	// Give raw position data, get estimated position
	k4abt_joint_t getNextPos(k4abt_joint_t);
	
	// set the measurement error of all 3 axis
	void setMeasurementError(float mea_e);

	// set the estimate error of all 3 axis
	void setEstimateError(float est_e);

	// set the processNoise of all 3 axis
	void setProcessNoise(float q);

private:
	SimpleKalmanFilter x = SimpleKalmanFilter(0.18, 0.1, 0.014);
	SimpleKalmanFilter y = SimpleKalmanFilter(0.18, 0.1, 0.014);
	SimpleKalmanFilter z = SimpleKalmanFilter(0.18, 0.1, 0.014);
	SimpleKalmanFilter qw = SimpleKalmanFilter(0.001, 0.1, 0.01);
	SimpleKalmanFilter qx = SimpleKalmanFilter(0.001, 0.001, 0.014);
	SimpleKalmanFilter qy = SimpleKalmanFilter(0.001, 0.001, 0.014);
	SimpleKalmanFilter qz = SimpleKalmanFilter(0.001, 0.001, 0.014);
};





#endif