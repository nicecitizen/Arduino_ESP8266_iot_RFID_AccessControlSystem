/*v1.02版本更新：增加了舵机在启动时复位成关门状态的改进，并增加开门停留时间*/
/*v1.03:修复了网页控制时因为只发送当前操作对象数值而引发的JSON解析导致功能紊乱BUG和，并缩短网络端刷新时间*/
/*ultimate version:修复了开门状态上传延迟和显示时间太短的bug，增加了密码开锁功能*/
/*
  ① 自动控制时，人体感应模块检测到有人、或者声音传感器检测到有声音，则打开门口的灯。手动控制时，不管传感器是否检测到人或声音，可以直接开关灯。
  ② 刷RFID卡开关门，信息正确则打开房门，信息不正确不开门。
  （1）可以通过网页进行远程监控。
  （2）远程端分别设置：
  ①本地控制/远程控制切换开关；
  ②手动控制/自动控制切换开关；
  ③远程手动控制时使用的设备总启动开关和总停止开关；设备端设置：本地手动控制时使用的设备总启动开关和总停止开关。
  设备端和远程端均需要直观显示当前设备的运行状况。本地控制时，远程端仅能进行监视、不能控制。
  本地控制分为手动控制和自动控制两种模式，当系统处于本地自动控制模式时，设备根据各传感器测量值和系统时间自动工作；
  当系统处于本地手动控制模式时，使用设备的手动输入元件和系统时间控制设备的动作。远程控制时，仅远程手动控制起作用、无远程自动控制模式。
  远程手动控制时，可以直观监视设备的运行状况，也可以通过网页遥控本地设备的动作。

*/
/*阿里云平台思路：上传的数据：控制状态（自动还是手动），是否有人，灯的开关，卡的合法性
  操控：控制状态，开关灯
*/
//Head Files
/*MACRO AND DEFINITION OF PIN AND GLOBAL VARIABLES*/
#include <ESP8266WiFi.h>   //ESP8266 WiFi芯片使用的头文件
#include "PubSubClient.h"  //通过MQTT协议上云平台时，需要用到的发布/订阅的头文件
#include <ArduinoJson.h>   //支持JSON数据格式的头文件，使用V5版本
//需要安装crypto库，设备连接阿里云平台时需要用户名和密码登录，且密码的生成有规则要求。crypto库中包含的SHA256.h是具有加密功能的头文件
#include "aliyun_mqtt.h"   //ESP8266针对阿里云连接开发的头文件，基于ESP8266官方SDK接入
#include <SPI.h>
#include <MFRC522.h>
#include "Servo.h"
#define RST_PIN     5           // 配置针脚
#define SS_PIN      4
#define LED LED_BUILTIN //使用板载LED IO0
#define objDctSgn 16 //人体感应检测模块
#define ledBtn A0
#define modeSwitchBtn 15
#define doorPin 2
MFRC522 mfrc522(SS_PIN, RST_PIN);   // 创建新的RFID实例
MFRC522::MIFARE_Key key;
Servo door;
int MODE = 0; //Auto:0,Manual:1
int Detected = 0; //Detected:1,Undetect:0
int ledStatus = 0;
int remoteLED = 0; //用于解决远程控制LED（手动模式）后被按钮获取方法覆盖的问题
int cardvalidity = 0; //for upload
int doorOpen = 0; //解决因为舵机延迟归位导致延迟上传开门状态的问题
String legalID = "2341346176";
String readID = "";
String legalPwd = "default";
String inputPwd = "";
//debunce
unsigned long lastDebounceTime1 = 0;
unsigned long lastDebounceTime2 = 0;
unsigned long debounceDelay = 750;
int lastBtn1 = 0;
int lastBtn2 = 0;
int currentBtn1 = 0;
int currentBtn2 = 0;
//macro fro Aliyun

#define WIFI_SSID        "HUAWEI P40 Pro"//替换自己的WIFI，2.4GHz频段
#define WIFI_PASSWD      "guodavid"//替换自己的WIFI，2.4GHz频段

#define PRODUCT_KEY      "ilywlySENGL" //阿里云IoT平台设备三元组信息，替换自己的PRODUCT_KEY
#define DEVICE_NAME      "rfidDoorSystem" //阿里云IoT平台设备三元组信息，替换自己的DEVICE_NAME
#define DEVICE_SECRET    "b7e38744ea38758d218f463111096bf7"//阿里云IoT平台设备三元组信息，替换自己的DEVICE_SECRET

#define DEV_VERSION       "1.0.0"        //固件版本信息

#define ALINK_BODY_FORMAT         "{\"id\":\"123\",\"version\":\"1.0.0\",\"method\":\"%s\",\"params\":%s}"    //MQTT消息数据的标准格式
#define ALINK_TOPIC_PROP_POST     "/sys/" PRODUCT_KEY "/" DEVICE_NAME "/thing/event/property/post"    //物模型通信Topic-设备属性上报（本地设备客户端发布消息）
#define ALINK_TOPIC_PROP_SET      "/sys/" PRODUCT_KEY "/" DEVICE_NAME "/thing/service/property/set"   //物模型通信Topic-设备属性设置（云端虚拟设备客户端发布消息，本地设备客户端订阅消息）
#define ALINK_METHOD_PROP_POST    "thing.event.property.post"    //设备属性上报的主题，本地设备（ESP8266）向阿里云IOT平台发布消息时使用
#define ALINK_TOPIC_DEV_INFO      "/ota/device/inform/" PRODUCT_KEY "/" DEVICE_NAME ""    //基础通信Topic-设备上报固件升级信息

unsigned long lastMs = 0;//用来保存上一次设备属性上报的时间

WiFiClient   espClient;    //创建客户端，代表本地设备：带有WiFi模组的ESP8266开发板
PubSubClient mqttClient(espClient);    //初始化构造器，代表本地设备将通过MQTT协议连接阿里云IoT平台
int upload_mode = 0;  //本地设备将要发布的数据之一
int upload_detect = 0; //本地设备将要发布的数据之二
int upload_cardvalidity = 0;

//aliyun code
//让ESP8266开发板能够接入WiFi网络
void init_wifi(const char *ssid, const char *password)
{
  WiFi.mode(WIFI_STA);    //设置模式，通过WiFi_STA还是WiFi_AP连网
  WiFi.begin(ssid, password);     //启动WiFi功能，开始连网。如果是STA模式，需要提供要连接的网络的ssid和password
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi does not connect, try again ...");
    delay(500);
  }//如果未连接上WiFi，串口每半分钟输出一个"WiFi does not connect, try again ..."

  Serial.println("Wifi is connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());//如果连接上WiFi，串口显示WiFi已连接以及ESP8266开发板的本地IP地址
}

//处理ESP8266向阿里云IOT平台订阅的其他客户端的主题
void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
  //如果云端虚拟设备发布了本地设备已订阅的主题的消息，串口显示有消息进来，并显示消息的主题（topic）和消息体（payload）
  //串口显示Message arrived [topic]，其中topic是消息的主题，具体内容是阿里云平台上任意可以订阅的主题，我们关心的是ALINK_TOPIC_PROP_SET
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.println("] ");
  //payload字符串长度不确定，将其放入StaticJsonBuffer后有剩余的部分用\0字符填充，并标识字符串结束
  payload[length] = '\0';
  Serial.println((char *)payload);//串口显示消息体（payload）的内容

  //在topic中检索ALINK_TOPIC_PROP_SET第一次出现的位置，如果topic中第一次出现了ALINK_TOPIC_PROP_SET，
  //返回ALINK_TOPIC_PROP_SET的地址；如果没有检索到ALINK_TOPIC_PROP_SET，返回NULL
  if (strstr(topic, ALINK_TOPIC_PROP_SET)) {
    //Json对象 对象树的内存工具 静态buffer
    //100是静态buffer的大小。如果这个Json对象更加复杂，那么就要根据需要去增加这个数值
    StaticJsonBuffer<200> jsonBuffer;

    //创建最外层的json对象：root对象，顶节点
    //解析JSON对象字符串，将JSON格式的payload消息拆分开
    JsonObject &root = jsonBuffer.parseObject(payload);
    // https://arduinojson.org/v5/assistant/  json数据解析网站
    int params_LightSwitch = root["params"]["LightSwitch"] | -1; //完成解析后，可以直接读取params中的各个变量参数值
    int params_ModeSwitch = root["params"]["upload_mode"] | -1;
    String params_legalPwd = root["params"]["legal_pwd"] ;
    String params_inputPwd = root["params"]["input_pwd"] ;
    if (params_legalPwd.length() > 0)
      legalPwd = params_legalPwd;
    else if (!params_inputPwd.length())
      Serial.println("设置密码为空请重新输入");
    if (params_inputPwd.length() > 0)
    {
      inputPwd = params_inputPwd;
      Serial.printf("%s %s\n", inputPwd, legalPwd);
    }
    else if (!params_legalPwd.length())
      Serial.println("输入密码为空请重新输入");
    //如果读到了所关心的变量，可以执行进一步的操作，这里是用LightSwitch变量开灯或关灯
    if (params_ModeSwitch == 1) //先切换模式再控制灯，防止出现同时更改灯和模式状态出现自动模式不能控制灯的情况
    {
      Serial.println("Switch to Manual Mode");
      MODE = 1;
    }
    else if (params_ModeSwitch == 0)
    {
      Serial.println("Switch to Auto Mode");
      MODE = 0;
    }
    if (!MODE)
    {
      MODE = 1;
      Serial.println("Switch to Manual Mode due to control remotely");
    }
    if (params_LightSwitch == 0)
    {
      Serial.println("led off");
      digitalWrite(LED, 1);
      remoteLED = 1;
    }
    else if (params_LightSwitch == 1)
    {
      Serial.println("led on");
      digitalWrite(LED, 0);
      remoteLED = 1;
    }

    if (!root.success())//如果解析没成功，串口输出解析失败（parseObject() failed）
    { Serial.println("parseObject() failed");
      return;
    }
  }
}

//ESP8266向阿里云IOT平台上报（发布）固件消息
void mqtt_version_post()
{
  char param[512];//用来存放ESP8266将要PUBLISH到服务器的消息

  sprintf(param, "{\"id\": 123,\"params\": {\"version\": \"%s\"}}", DEV_VERSION);//将要上报的设备固件版本信息格式化成标准报文格式后，写入param字符串中
  Serial.println(param);
  mqttClient.publish(ALINK_TOPIC_DEV_INFO, param);//ESP8266向阿里云IOT平台PUBLISH（发布）消息，主题是ALINK_TOPIC_DEV_INFO，消息内容是param
}

//ESP8266连接阿里云IOT平台
void mqtt_check_connect()
{
  while (!mqttClient.connected())//如果ESP8266开发板没连上阿里云IOT平台，那就继续连
  {
    while (connect_aliyun_mqtt(mqttClient, PRODUCT_KEY, DEVICE_NAME, DEVICE_SECRET))//只处理连上了的情况
    {
      Serial.println("MQTT connect succeed!");
      mqttClient.subscribe(ALINK_TOPIC_PROP_SET);//ESP8266开发板从阿里云IOT平台SUBSCRIBE（订阅）消息，主题是ALINK_TOPIC_PROP_SET
      Serial.println("subscribe done");
      mqtt_version_post();//ESP8266向阿里云IOT平台上报（发布）固件消息
    }
  }
}

//ESP8266向阿里云IOT平台上报本地数据
void mqtt_interval_post()
{
  char param[512];
  char jsonBuf[1024];//用来存放ESP8266将要PUBLISH到服务器的消息

  sprintf(param, "{\"LightSwitch\":%d,\"upload_mode\":%d,\"upload_cardvalidity\":%d,\"upload_detect\":%d}", !digitalRead(LED), upload_mode, upload_cardvalidity, upload_detect); //将要上报的数据格式化后写入param字符串中
  sprintf(jsonBuf, ALINK_BODY_FORMAT, ALINK_METHOD_PROP_POST, param);//将要上报的数据加上主题，一起被格式化成标准报文格式后，写入jsonBuf字符串中
  Serial.println(jsonBuf);//串口输出将要上传的报文
  mqttClient.publish(ALINK_TOPIC_PROP_POST, jsonBuf);//ESP8266向阿里云IOT平台PUBLISH（发布）消息，主题是ALINK_TOPIC_PROP_POST，消息内容是jsonBuf
  //需要注意：阿里云IOT平台的工作机制是所有的本地数据都需要先上报，然后其中的读写型数据才能从云平台上向本地进行参数的设置
}


//function
void ledControl()
{

  if (MODE) //手动控制
  {
    //Serial.printf("AnalogRead LEDBTN %d\n",analogRead(ledBtn));
    currentBtn2 = (analogRead(ledBtn) == 1024) ? 1 : 0;
    if (currentBtn2 != lastBtn2)
    {
      if (millis() - lastDebounceTime2 > debounceDelay)
      {
        if (currentBtn2)
        {
          Serial.println("LED Control Button pressed");
          ledStatus ^= 1;
          remoteLED = 0;
        }
        lastDebounceTime2 = millis();
      }
    }
    if (!remoteLED)
      digitalWrite(LED, !ledStatus);
  }
  else
    digitalWrite(LED, !Detected);
}

void doorControl(int op)
{
  int pos = 0;
  int cur = door.read();
  if (op) //开门
  {
    if (cur >= 90)
    {
      Serial.println("Door has already been opened! ");
      return ;
    }
    for (pos = cur; pos <= 90; pos++)
    {
      door.write(pos);
      delay(10);
    }
  }
  else
  {
    if (cur <= 0)
    {
      Serial.println("Door has already been closed! ");
      return ;
    }
    for (pos = cur; pos >= 0; pos--)
    {
      door.write(pos);
      delay(10);
    }
  }
}
void readCard()
{
  // 寻找新卡
  if ( ! mfrc522.PICC_IsNewCardPresent()) {
    //Serial.println("没有找到卡");
    return;
  }

  // 选择一张卡
  if ( ! mfrc522.PICC_ReadCardSerial()) {
    Serial.println("没有卡可选");
    return;
  }


  // 显示卡片的详细信息
  Serial.print(F("卡片 UID:"));
  dump_byte_array(mfrc522.uid.uidByte, mfrc522.uid.size);
  Serial.println();
  Serial.print(F("卡片类型: "));
  MFRC522::PICC_Type piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  Serial.println(mfrc522.PICC_GetTypeName(piccType));

  // 检查兼容性
  if (    piccType != MFRC522::PICC_TYPE_MIFARE_MINI
          &&  piccType != MFRC522::PICC_TYPE_MIFARE_1K
          &&  piccType != MFRC522::PICC_TYPE_MIFARE_4K) {
    Serial.println(F("仅仅适合Mifare Classic卡的读写"));
    return;
  }

  MFRC522::StatusCode status;
  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("身份验证失败？或者是卡链接失败"));
    Serial.println(mfrc522.GetStatusCodeName(status));
    return;
  }
  //停止 PICC
  mfrc522.PICC_HaltA();
  //停止加密PCD
  mfrc522.PCD_StopCrypto1();
  return;
}
/**
   将字节数组转储为串行的十六进制值
*/
void dump_byte_array(byte *buffer, byte bufferSize)
{
  readID = "";
  for (byte i = 0; i < bufferSize; i++)
  {
    //将uid转成字符串
    readID += buffer[i];
    Serial.print(buffer[i] < 0x10 ? " 0" : " ");
    Serial.print(buffer[i], HEX);
  }
  Serial.println('\n');
  Serial.print(readID);
}

void aliyunUpload()//在loop中上传数据
{
  if (millis() - lastMs >= 1000 || doorOpen) //每5秒读取一次本地数据
  {
    lastMs = millis();

    mqtt_check_connect();//连接阿里云IOT平台
    //将进行格式转换并串口显示
    upload_mode = MODE;
    upload_cardvalidity = cardvalidity;
    upload_detect = Detected;
    mqtt_interval_post();//ESP8266向阿里云IOT平台上报本地数据
  }

  mqttClient.loop();
}

void modeSwitch()
{
  currentBtn1 = digitalRead(modeSwitchBtn);
  if (lastBtn1 != currentBtn1) //模式切换按钮被按下，切换模式
  {
    if (millis() - lastDebounceTime1 > debounceDelay)
    {
      if (currentBtn1)
      {
        //Serial.println("ModeSwitchButton Pressed");
        MODE ^= 1;
      }
      lastDebounceTime1 = millis();
    }
  }
}
void accessControl()
{
  readCard();
  if (readID == legalID || legalPwd == inputPwd)
  {
    Serial.println("OK,Open door!\n");
    cardvalidity = 1;
    doorOpen = 1;
    aliyunUpload();
    doorOpen = 0;
    doorControl(1);
    delay(3000);
    doorControl(0);
    readID = "";
    inputPwd = "";
  }
  else
  {
    cardvalidity = 0;
    //Serial.println("Card Not Match!\n");
  }
}
//main function
void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  door.attach(doorPin);
  pinMode(objDctSgn, INPUT); //人体感应检测信号
  pinMode(ledBtn, INPUT); //手动控制下开关灯按钮
  pinMode(modeSwitchBtn, INPUT); //模式切换按钮
  pinMode(LED, OUTPUT);
  SPI.begin();        // SPI开始
  mfrc522.PCD_Init(); // Init MFRC522 card
  init_wifi(WIFI_SSID, WIFI_PASSWD);//连接WiFi
  mqttClient.setCallback(mqtt_callback);//接收并处理来自阿里云IOT平台的消息
  door.write(0);//舵机启动时复位
}


void loop()
{
  // put your main code here, to run repeatedly:
  modeSwitch();
  Detected = digitalRead(objDctSgn);
  //Serial.println("MODE(0 for auto): ");
  //Serial.printf(" %d\n ",MODE);
  //Serial.println("ObjDct(1 for detected) ");
  //Serial.printf("%d\n",Detected);
  ledControl();
  // test digitalWrite(LED,!Detected);
  accessControl();
  aliyunUpload();
}
