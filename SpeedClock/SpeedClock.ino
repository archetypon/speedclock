//-------------------------------------------------------------------------------
// Licence:
// Copyright (c) 2019 Luzzi Valerio 
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//
//
// Name:         SpeedClock.ino
// Purpose:      A clock for speed climb
//
// Author:      Luzzi Valerio
//
// Created:     20/03/2019
//-------------------------------------------------------------------------------

#include <SPI.h>
//#include <WiFi101.h> //MKR1000
#include <WiFiNINA.h> //MKR1010
#include <WiFiUdp.h>
#include "HX711.h"
#include "arduino_secrets.h" 

// HX711 -----------------------
const int LOADCELL_DOUT_PIN = 4;
const int LOADCELL_SCK_PIN  = 3;
const int BUTTON_PIN = 2;
#define ID "0" 


HX711 scale;
long tare; 
long weight=0;
long max_weight = 0;
const unsigned long THRESHOLD_WEIGHT = 15;  //15Kg
const unsigned long ALPHA = 5200;  //conversion factor for scale into kg.
const unsigned long WEIGHTING_INTERVAL = 250000;
const int DEBUG = 0;
//------------------------------
unsigned long t0;
unsigned long t1;
unsigned long LOOP_TIME;
unsigned long RUNNING_COUNTER;

unsigned long mills_in_this_state =0;


enum state {
  BEGIN,
  CALIBRATING,
  IDLE,
  PREARMED,
  ARMED,
  RUNNING,
};

state STATE;

//--------------------------------------------------------------------------------------------
//
//    +-------+     +-------------+       +------+       +----------+       +---------+       +---------+
//    |BEGIN  |---->|CALIBRATING  |------>| IDLE |------>| PREARMED |------>| ARMED   |------>| RUNNING |
//    +-------+     +-------------+       +------+       +----------+       +---------+       +---------+     
//                                           ^              |                 |                 |          
//                                           |              |                 |                 |         
//                                           +--------------+-----------------+-----------------+
//
//------------ WiFI --------------------------------------------------------------------------
int status = WL_IDLE_STATUS;
///////please enter your sensitive data in the Secret tab/arduino_secrets.h
char ssid[] = SECRET_SSID_1;        // your network SSID (name)
char pass[] = SECRET_PASS_1;        // your network password (use for WPA, or use as key for WEP)
unsigned int localPort  = 10999;      // local port to listen on
unsigned int remotePort = 11000;      // remote display port
char cmd[32]; //buffer command to broadcast  
char packetBuffer[255]; //buffer to hold incoming packet

WiFiUDP Udp;
//WiFiServer wifiServer(80);
//IPAddress broadcastIp(192,168,1,255);
IPAddress broadcastIp(255,255,255,255);

void setup() {

  changeStateTo(BEGIN);

  // initialize the button pin as a input:
  pinMode(BUTTON_PIN, INPUT);
  
  //Initialize serial and wait for port to open:
  Serial.begin(9600);

  // check for the presence of the shield:
  while (WiFi.status() == WL_NO_SHIELD) {
      println("WiFi shield not present");
      delay(1000);
  }

  // attempt to connect to WiFi network:
  while ( status != WL_CONNECTED) {
    print("Attempting to connect to SSID: ");
    println(ssid);
    status = WiFi.begin(SECRET_SSID_1, SECRET_PASS_1);
    if (status==4){
      status = WiFi.begin(SECRET_SSID_2, SECRET_PASS_2);
    }
    // wait 10 seconds for connection:
    println("------------------------------------------------");
    delay(2000);
  }
  println("Connected to wifi");
  printWiFiStatus();

 

  println("\nStarting connection to server...");
  // if you get a connection, report back via serial:
  Udp.begin(localPort);

  //---------- WEIGHT SCALE -----------------
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
 
  changeStateTo(CALIBRATING);
  print("tara= ");
  tare = taring();
  //println(tare);
  changeStateTo(IDLE);

}

void loop() {

      //weighting ~= 85ms
      LOOP_TIME = millis();
      if (STATE!=RUNNING || (STATE==RUNNING && RUNNING_COUNTER%WEIGHTING_INTERVAL==0)){
          while ( !scale.is_ready() ){
              delay(1);  
          }
          weight = abs(scale.read()-tare) / ALPHA;
          weight = (weight>200)?0:weight;  //caseof crazy values
          max_weight = max(weight,max_weight);
          RUNNING_COUNTER=0;
          
          if (weight>2){
            print(weight);print("  ");println(max_weight);
          }
      }
      RUNNING_COUNTER++;

      //Periodically broadcast a i_m_alive message
      if (STATE == IDLE && mills_in_this_state>=2500){
          changeStateTo(IDLE);
      }
      
      //PRE-ARMED!!! transient state for 500ms before becaming ARMED or falling back in IDLE
      if (STATE == IDLE && weight>=THRESHOLD_WEIGHT){
          changeStateTo(PREARMED);
      }

      //ARMED
      if (STATE == PREARMED && mills_in_this_state>=500){
          changeStateTo(ARMED);
      }

      //RETURN TO IDLE
      if (STATE == PREARMED && weight<THRESHOLD_WEIGHT){
          changeStateTo(IDLE);
      }

      //START ---> RUNNING.................................
      if (STATE == ARMED && weight<max_weight/2 && max_weight>0){
          t0 = millis();
          changeStateTo(RUNNING);
          weight=0;
          max_weight=0;
      }

      //Periodically broadcast a i_m_alive message
      if (STATE==ARMED && mills_in_this_state>1000){
          changeStateTo(ARMED);
      } 

      //BREAK RUNNING STATE BY GETTING ON THE PLATFORM 
      if (STATE==RUNNING && weight>=THRESHOLD_WEIGHT){
          STATE = IDLE;
          mills_in_this_state=0;
          sprintf(cmd,"%s:stop:0",ID);
          repeated_broadcast(cmd,3);
          println(cmd);
          //broadcast(cmd);
          //println(cmd);
      }

      //STOP RUNNING BY PRESSING BUTTON 
      if (STATE==RUNNING && digitalRead(BUTTON_PIN)){
          t1 = millis();
          STATE = IDLE;
          mills_in_this_state=0;
          sprintf(cmd,"%s:stop:%d",ID,(t1-t0));
          repeated_broadcast(cmd,3);
          //broadcast(cmd);
          max_weight=0;
      }

      //UDP RECEIVE
      if (true){
          int packetSize = Udp.parsePacket();
          if (packetSize){
              int len = Udp.read(packetBuffer, 255);
              if (len > 0) {packetBuffer[len] = 0;}
              if (strstr(packetBuffer,":stop:")){
                changeStateTo(IDLE);
              }
          }
      }
      LOOP_TIME = millis()-LOOP_TIME;
      mills_in_this_state+=LOOP_TIME;
}//loop

long taring(){
  
  long times = 50;
  long mean = 0;
  for(int j=0;j < times; j++){
    while ( !scale.is_ready() ){
        yield();  
    }
    mean += scale.read();
    delay(100);
  } 
  return mean/times;
}

void broadcast(char* text){

    Udp.beginPacket(broadcastIp, remotePort);
    Udp.write(text);
    Udp.endPacket();
    //println(text);
}

void repeated_broadcast(char* text, int n){

    for(int j=1;j<=n;j++){
      Udp.beginPacket(broadcastIp, remotePort);
      Udp.write(text);
      Udp.endPacket();
      if (j>1){
        delay(j*random(50,300));
      }
    }
}

void changeStateTo(state s){
  
    STATE = s;
    mills_in_this_state = 0;
    switch (s) {

      case BEGIN:
        sprintf(cmd,"%s:begin:0",ID);
        break;
      case CALIBRATING:
        sprintf(cmd,"%s:calibrating:0",ID);
        break;
      case IDLE:
        sprintf(cmd,"%s:ready:0",ID);
        break;
      case PREARMED:
        sprintf(cmd,"%s:prearmed:0",ID);
        break;
      case ARMED:
        sprintf(cmd,"%s:armed:0",ID);     
        break;
      case RUNNING:
        sprintf(cmd,"%s:start:0",ID);
        break;
      default:
        sprintf(cmd,"%s:ready:0",ID);
        
    }
    
    broadcast(cmd);
    println(cmd);
}

void print(const char* text){
  if (DEBUG)Serial.print(text);
}

void print(int text){
  if (DEBUG)Serial.print(text);
}

void print(long text){
  if (DEBUG)Serial.print(text);
}

void print(float text){
  if (DEBUG)Serial.print(text);
}

void println(const char* text){
  if (DEBUG)Serial.println(text);  
}

void println(int text){
  if (DEBUG)Serial.println(text);  
}

void println(long text){
  if (DEBUG)Serial.println(text);  
}

void println(float text){
  if (DEBUG)Serial.println(text);  
}


void printWiFiStatus() {
  // print the SSID of the network you're attached to:
  print("SSID: ");
  println(WiFi.SSID());

  // print your WiFi shield's IP address:
  IPAddress ip = WiFi.localIP();
  print("IP Address: ");
  if (DEBUG) Serial.println(ip);

  // print the received signal strength:
  long rssi = WiFi.RSSI();
  print("signal strength (RSSI):");
  print(rssi);
  println(" dBm");
}
