#include "main.hpp"
//airsim
#include "vehicles/multirotor/api/MultirotorRpcLibClient.hpp"
#include "api/RpcLibClientBase.hpp"
//opencv
#include "opencv2/opencv.hpp"

#include <iostream>
#include "WINDOWS.h"

#include "timer.h" 
#include <thread>
#include "getImg.h" //深度图处理


//命名空间
using namespace std;
using namespace cv;
using namespace msr::airlib;

typedef ImageCaptureBase::ImageRequest ImageRequest;	//图像请求
typedef ImageCaptureBase::ImageResponse ImageResponse;	//图像响应
typedef ImageCaptureBase::ImageType ImageType;			//图像类型

// V 共享变量 ****************************************
//airsim 相关
msr::airlib::MultirotorRpcLibClient client("localhost", 41451, 60000);//连接localhost:41451 	
//传感器数据
//GpsData			 GPS_data;			//GPS
BarometerData	 Barometer_data;	//气压计
MagnetometerData Magnetometer_data;	//磁力计
ImuData			 Imu_data;			//IMU
std::mutex g_mutex_img;//互斥锁
std::mutex g_mutex_sensor;//传感器数据互斥锁，用于气压计、磁力计、IMU数据
std::mutex g_mutex_control_cmd;//控制命令数据锁
int avoid_obstacle_flag = 0; //避障标志

int flag_mode = 1;//  1表示任务1钻圈，2表示任务2小树林

//图像
cv::Mat front_image, down_image, depth_image;

// A 共享变量 ****************************************//



// V 共享函数 ****************************************

// A 共享函数 ****************************************//


//线程
//创建可等候定时器
HANDLE hTimer_Key_Scan = CreateWaitableTimer(NULL, FALSE, NULL);
HANDLE hTimer_get_img = CreateWaitableTimer(NULL, FALSE, NULL);
HANDLE hTimer_get_sensor = CreateWaitableTimer(NULL, FALSE, NULL);
HANDLE hTimer_move_control = CreateWaitableTimer(NULL, FALSE, NULL);
//线程函数声明
void Key_Scan(void);
void get_img(void);
void get_sensor(void);//获取传感器数据
void move_control(void);//移动控制线程


static int key_control(int key);//按键控制函数声明

//全局标志位
static bool flag_exit = 0;//如果未true则表示推出程序

int main()
{
	while (RpcLibClientBase::ConnectionState::Connected != client.getConnectionState())
		client.confirmConnection();//连接场景

	while (!client.isApiControlEnabled())
		client.enableApiControl(true);//获取api控制

	client.armDisarm(true);//解锁飞控	  
	client.hover();//hover模式
	
	//设置定时器时间
	INT64 nDueTime = -0 * _SECOND;//定时器生效时间，立即
	SetWaitableTimer(hTimer_Key_Scan, (PLARGE_INTEGER)&nDueTime, 50, NULL, NULL, FALSE);//50表示定时器周期50ms
	SetWaitableTimer(hTimer_get_img, (PLARGE_INTEGER)&nDueTime, 400, NULL, NULL, FALSE);//50表示定时器周期50ms
	SetWaitableTimer(hTimer_get_sensor, (PLARGE_INTEGER)&nDueTime, 50, NULL, NULL, FALSE);//50表示定时器周期50ms
	SetWaitableTimer(hTimer_move_control, (PLARGE_INTEGER)&nDueTime, 50, NULL, NULL, FALSE);//50表示定时器周期50ms
	
	printf("线程初始化\n");
	std::thread t_Key_Scan(Key_Scan);
	std::thread t_get_img(get_img);
	std::thread t_get_sensor(get_sensor);
	std::thread t_move_control(move_control);

	printf("线程初始化完成\n");
	t_Key_Scan.join(); //阻塞，等待该线程退出
	t_get_img.join();
	t_get_sensor.join();
	t_move_control.join();
	//关闭定时器
	CloseHandle(hTimer_Key_Scan);
	CloseHandle(hTimer_get_img);
	CloseHandle(hTimer_get_sensor);
	CloseHandle(hTimer_move_control);
	
	//摧毁所有OpenCV窗口
	cv::destroyAllWindows();
	
	printf("所有线程退出，程序结束\n");
	return 0;
}


void Key_Scan(void)
{
	int key_value_cv = -1;
	clock_t time_1 = clock();//get time
	
	while (true)
	{
		//printf("Key_Scan耗时:%d ms\n", clock() - time_1);
		time_1 = clock();
		//等待定时器时间到达
		WaitForSingleObject(hTimer_Key_Scan, INFINITE);
		if (flag_exit)//退出线程
		{
			return;
		}

		//显示图像
		g_mutex_img.lock();//获得锁
		if (!front_image.empty())
		{
			cv::imshow("FROWARD", front_image);
		}
		if (!down_image.empty())
		{
			cv::imshow("DOWN", down_image);
		}
		if (!depth_image.empty())
		{
			cv::imshow("FROWARD_DEPTH", depth_image);
		}
		g_mutex_img.unlock();//释放锁

		key_value_cv = cv::waitKeyEx(1);//读取按键
		while (-1 != cv::waitKey(1));	//显示图像，要把缓冲区读完后，才会显示1ms图像
		key_control(key_value_cv);		//执行按键功能
	
	}
	
}

void get_img(void)
{
	int i = 0;
	clock_t time_1= clock();//get time

	std::vector<ImageRequest> request = { ImageRequest(0, ImageType::Scene) , ImageRequest(0, ImageType::DepthPerspective, true), ImageRequest(3, ImageType::Scene) };
	while (true)
	{

		//等待定时器时间到达
		WaitForSingleObject(hTimer_get_img, INFINITE);

		time_1 = clock();
		if (flag_exit)//退出线程
		{
			return;
		}

		std::vector<ImageResponse>& response = client.simGetImages(request);//获取图像
		if (response.size() > 0)
		{
			g_mutex_img.lock();//获得锁
			front_image = cv::imdecode(response.at(0).image_data_uint8, cv::IMREAD_COLOR);	//;前视图
			down_image = cv::imdecode(response.at(2).image_data_uint8, cv::IMREAD_COLOR);	//下视图	
			//深度图
			depth_image = Mat(response.at(1).height, response.at(1).width, CV_16UC1);
			// 转换深度图获得浮点数的深度图
			avoid_obstacle_flag = imageResponse2Mat(response.at(1), depth_image);//返回值为距离

			g_mutex_img.unlock();//释放锁
		}

		//printf("    get_img耗时:%d ms\n", clock() - time_1);

// 分割线 ***************************************************************************************************


		switch (flag_mode)//任务1和任务2
		{
		default:
		case 1://任务1 钻圈
			
			//调用 钻圈函数();
			break;
		case 2://任务2 小树林
			
			//调用 小树林搜索函数();
			break;
		}


	}
	
}

struct control_cmd {
	bool flag_enable = false;//true表示该指令没执行过,需要执行一次
	int flag_move = 0;// 0是停止，1是pitch/roll模式移动，2是inclination/orientation模式移动，3是设定高度
	float pitch = .0f, roll = .0f;
	float inclination = .0f, orientation = .0f;
	float duration = .05f;				//持续时间
	float set_H = .0f;//设定高度
	float yaw_rate = 0.0f;

};//放头文件里
struct control_cmd control_cmdset;
bool flag_H = false;//指示顶高是否完成，ture表示定高完成
//设置控制命令
//void set_control_cmd(bool flag_enable=true, int flag_move=0, float pitch = .0f, float roll = .0f,
//					 float inclination = .0f, float orientation = .0f, float duration=0.05f, 
//					 float set_H = .0f, float yaw_rate = 0.0f)
void set_control_cmd(bool flag_enable, int flag_move, float pitch, float roll,
					 float inclination, float orientation, float duration,
					 float set_H, float yaw_rate)
{
	g_mutex_control_cmd.lock();//加锁

	control_cmdset.flag_enable		= flag_enable;
	control_cmdset.flag_move		= flag_move;
	control_cmdset.pitch			= pitch;
	control_cmdset.roll				= roll;
	control_cmdset.inclination		= inclination;
	control_cmdset.orientation		= orientation;
	control_cmdset.duration			= duration;
	control_cmdset.set_H			= set_H;
	control_cmdset.yaw_rate			= yaw_rate;

	g_mutex_control_cmd.unlock();//释放锁
}
void move_control(void)//移动控制线程
{
	clock_t time_1= clock();//get time
	
	float throttle = 0.587402f;//刚好抵消重力时的油门
	struct control_cmd control_cmdset_temp;
	while (true)
	{
		//等待定时器时间到达
		WaitForSingleObject(hTimer_move_control, INFINITE);
		if (flag_exit)//退出线程
		{
			return;
		}

		
		
		g_mutex_control_cmd.lock();//加锁
		control_cmdset_temp.flag_enable		= control_cmdset.flag_enable;
		control_cmdset_temp.flag_move		= control_cmdset.flag_move;
		control_cmdset_temp.pitch			= control_cmdset.pitch;
		control_cmdset_temp.roll			= control_cmdset.roll;
		control_cmdset_temp.inclination		= control_cmdset.inclination;
		control_cmdset_temp.orientation		= control_cmdset.orientation;
		control_cmdset_temp.duration		= control_cmdset.duration;
		control_cmdset_temp.set_H			= control_cmdset.set_H;
		control_cmdset_temp.yaw_rate		= control_cmdset.yaw_rate;

		control_cmdset.flag_enable = false;
		g_mutex_control_cmd.unlock();//释放锁
		if (control_cmdset_temp.flag_enable)
		{
			switch (control_cmdset_temp.flag_move)
			{
			default:
			case 0:// 0是停止
				client.hover();//hover
				break;
			case 1://1是pitch/roll模式移动
				client.hover();//hover
				throttle = 0.587402f / cos(control_cmdset_temp.pitch) / cos(control_cmdset_temp.roll);
				client.moveByAngleThrottle(control_cmdset_temp.pitch, control_cmdset_temp.roll, throttle, control_cmdset_temp.yaw_rate, control_cmdset_temp.duration);
				break;
			case 2://2是inclination / orientation模式移动
				break;
			case 3://3是设定高度
				client.hover();//hover
				// 控制高度函数();
				//
				//
				//
				//
				break;
			}
		}
	}


}
static int key_control(int key)//按键控制
{
	clock_t time_1;// = clock();//get time
	//float roll = 0.1f;//绕x轴逆时针 //单位是弧度
	//float pitch = 0.1f;//绕y轴逆时针  
	//float yaw = 0.0f; //绕z轴逆时针
	//float duration = 0.05f;//持续时间
	//float throttle = 0.587402f;//刚好抵消重力时的油门
	//float yaw_rate = 0.1f;

	float roll = 0.0f;//绕x轴逆时针 //单位是弧度
	float pitch = 0.0f;//绕y轴逆时针  
	float yaw = 0.0f; //绕z轴逆时针
	float duration = 0.05f;//持续时间
	float throttle = 0.587402f;//刚好抵消重力时的油门
	float yaw_rate = 0.0f;

	bool flag = false;//为真时执行控制函数
	switch (key)
	{
	case 27://ESC
		flag_exit = true;//退出标志位置位
		break;
	case 32://空格
		client.hover();//hover模式
		printf("hover\n");
		//flag = true;
		break;
	case 'w':
		flag = true;
		throttle += 0.1f;
		break;
	case 's':
		flag = true;
		throttle -= 0.1f;
		break;
	case 'a'://旋转时会下降...
		set_control_cmd(true, 1, .0f, .0f, .0f, .0f, 0.05f, .0f, -0.7f);
		break;
	case 'd':
		set_control_cmd(true, 1, .0f, .0f, .0f, .0f, 0.05f, .0f, 0.7f);
		break;

	//下面是以机头方向前后左右
	case 'i'://pitch y轴逆时针角度
		set_control_cmd(true, 1, -0.1f, .0f, .0f, .0f, 0.05f, .0f, .0f);
		break;
	case 'k'://pitch y轴逆时针角度
		set_control_cmd(true, 1, 0.1f, .0f, .0f, .0f, 0.05f, .0f, .0f);

		break;
	case 'j'://roll x轴逆时针角度
		set_control_cmd(true, 1, .0f, -0.1f, .0f, .0f, 0.05f, .0f, .0f);
		break;
	case 'l'://roll x轴逆时针角度
		set_control_cmd(true, 1, .0f, 0.1f, .0f, .0f, 0.05f, .0f, .0f);
		break;
	case 2490368://方向键上
		throttle += 0.0003f;
		printf("throttle=%f\n", throttle);
		break;
	case 2621440://方向键下

		throttle -= 0.0003f;
		printf("throttle=%f\n", throttle);

		break;
	default:
		break;
	}

	if (flag)
	{
		flag = false;
		client.moveByAngleThrottle(pitch, roll, throttle, yaw_rate, duration);
	}

	return 0;
}

void get_sensor(void)//获取传感器数据
{
	clock_t time_1 = clock();
	while (true)
	{
		//printf("  get_sensor耗时:%d ms\n", clock() - time_1);
		time_1 = clock();
		//等待定时器时间到达
		WaitForSingleObject(hTimer_get_sensor, INFINITE);
		if (flag_exit)//退出线程
		{
			return;
		}

		//更新传感器数据
		g_mutex_sensor.lock();//传感器数据锁
		Barometer_data = client.getBarometerdata();
		Magnetometer_data = client.getMagnetometerdata();
		Imu_data = client.getImudata();
		g_mutex_sensor.unlock();//传感器数据锁释放	
	}
}






