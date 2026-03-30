#include <WiFi.h>
#include <FirebaseESP32.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ESP32Servo.h>

// ==========================================
// CONFIGURATION
// ==========================================
#define WIFI_SSID "HOME"
#define WIFI_PASSWORD "Project@123"

#define FIREBASE_HOST "smart-tollgate-78526-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "xTAsqwist9k8BPq1LhjgIdQjsALgncP5pUGNvOg2" 

// ==========================================
// PIN DEFINITIONS 
// ==========================================
#define SS_PIN 5      // RFID SS/SDA
#define RST_PIN -1    // RFID RST (Tied to 3.3V)
#define SERVO_PIN 15  // Servo Signal

// GSM Pins (Using ESP32 Hardware Serial 2)
#define GSM_RX_PIN 16 // Connect to SIM900A TX
#define GSM_TX_PIN 17 // Connect to SIM900A RX

// ==========================================
// COMPONENT INITIALIZATION
// ==========================================
#define i2c_Address 0x3c 
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

MFRC522 rfid(SS_PIN, RST_PIN);
Servo gateServo;

// FIREBASE OBJECTS 
FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;

// ==========================================
// HELPER FUNCTIONS
// ==========================================
// Basic display for simple messages
void showOnOLED(String line1, String line2) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 15);
  display.println(line1);
  display.setCursor(0, 35);
  display.setTextSize(2); 
  display.println(line2);
  display.display();
}

// Detailed display for successful transactions
void showTransactionDetails(String name, String vehId, String vType, float fee, float bal) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  
  // Header
  display.setCursor(0, 0);
  display.println("--- TOLL CLEARED ---");
  
  // Details list (spaced out vertically)
  display.setCursor(0, 14);
  display.println("Name: " + name);
  
  display.setCursor(0, 24);
  display.println("Veh : " + vehId + " [" + vType + "]");
  
  display.setCursor(0, 34);
  display.println("Paid: Rs." + String(fee));
  
  display.setCursor(0, 44);
  display.println("Bal : Rs." + String(bal));
  
  display.display();
}

void sendSMS(String phoneNumber, String message) {
  Serial.println("Sending SMS to: " + phoneNumber);
  
  Serial2.println("AT+CMGF=1"); // Set GSM to text mode
  delay(1000);
  
  Serial2.print("AT+CMGS=\"");
  Serial2.print(phoneNumber);
  Serial2.println("\"");
  delay(1000);
  
  Serial2.print(message); // The SMS body
  delay(500);
  
  Serial2.write(26); // ASCII code for CTRL+Z to send the message
  delay(3000); // Wait for the network to process
  
  Serial.println("SMS Sent Successfully!");
}

// NEW: Network Check & Auto-Reconnect Function
void checkNetwork() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost! Stopping operations...");
    showOnOLED("Network Lost", "Connecting..");
    
    // Disconnect and try to reconnect
    WiFi.disconnect();
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    // Block all operations until Wi-Fi is back
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    
    Serial.println("\nWiFi Reconnected!");
    showOnOLED("System Ready", "Scan Card");
  }
}

// ==========================================
// MAIN SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  
  // Initialize GSM Serial Port
  Serial2.begin(9600, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN); 
  
  SPI.begin();
  rfid.PCD_Init();
  
  ESP32PWM::allocateTimer(0);
  gateServo.setPeriodHertz(50); 
  gateServo.attach(SERVO_PIN, 500, 2400); 
  gateServo.write(0); // Close gate initially

  delay(250); 
  if(!display.begin(i2c_Address, true)) {
    Serial.println("SH1106 allocation failed");
    for(;;);
  }
  display.clearDisplay();
  display.display();

  // 1. Connect to Wi-Fi
  showOnOLED("Connecting...", "WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  // 2. Connect to Firebase
  config.database_url = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  // Tell Firebase library to auto-reconnect its internal sockets when Wi-Fi drops
  Firebase.reconnectWiFi(true);

  // 3. Initialize GSM Module
  showOnOLED("Init GSM...", "Please Wait");
  Serial2.println("AT"); // Wake up GSM
  delay(1000);

  showOnOLED("System Ready", "Scan Card");
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  // 1. Ensure we are connected to Wi-Fi before doing anything else
  // If disconnected, this function will block until connected again.
  checkNetwork();

  // 2. Wait for a new card
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }

  // 3. Get UID as a String
  String cardUID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    cardUID += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    cardUID += String(rfid.uid.uidByte[i], HEX);
  }
  cardUID.toUpperCase();
  
  Serial.println("Card Scanned: " + cardUID);
  showOnOLED("Processing...", cardUID);

  // 4. Process the toll transaction
  processTransaction(cardUID);

  // 5. Halt card to prevent continuous reading
  rfid.PICC_HaltA();
  
  delay(3000);
  
  // Double check network before resetting screen to Ready (in case it dropped during delay)
  if (WiFi.status() == WL_CONNECTED) {
    showOnOLED("System Ready", "Scan Card");
  }
}

// ==========================================
// TRANSACTION LOGIC
// ==========================================
void processTransaction(String uid) {
  String basePath = "/Toll_Users/" + uid;

  // 1. Check if User exists by trying to fetch their vehicle type
  String vehicleType = "";
  if (Firebase.getString(firebaseData, basePath + "/vehicle_type")) {
    vehicleType = firebaseData.stringData();
  } else {
    showOnOLED("Access Denied", "Unknown Card");
    Serial.println("Card not found or missing vehicle_type");
    return;
  }

  // 2. Map vehicle type to the Firebase prices path 
  String priceKey = vehicleType;
  priceKey.toLowerCase(); 

  // 3. Fetch the live Toll Fee for this vehicle type
  float dynamicTollFee = 0.0;
  if (Firebase.getFloat(firebaseData, "/prices/" + priceKey)) {
    dynamicTollFee = firebaseData.floatData();
  } else if (Firebase.getInt(firebaseData, "/prices/" + priceKey)) {
    dynamicTollFee = (float)firebaseData.intData();
  } else {
    Serial.println("Failed to fetch price for " + priceKey);
    showOnOLED("System Error", "Price Missing");
    return; 
  }

  Serial.println("Vehicle: " + vehicleType + " | Toll Fee: Rs." + String(dynamicTollFee));

  // 4. Fetch User Balance
  if (Firebase.getFloat(firebaseData, basePath + "/balance") || Firebase.getInt(firebaseData, basePath + "/balance")) {
    float currentBalance = 0.0;
    
    if (firebaseData.dataType() == "int") {
      currentBalance = (float)firebaseData.intData();
    } else {
      currentBalance = firebaseData.floatData();
    }

    if (currentBalance >= dynamicTollFee) {
      // 5. Deduct Balance
      float newBalance = currentBalance - dynamicTollFee;
      Firebase.setFloat(firebaseData, basePath + "/balance", newBalance);

      // 6. Fetch User's Name
      String displayName = "User";
      if (Firebase.getString(firebaseData, basePath + "/name")) {
        String fullName = firebaseData.stringData();
        int spaceIndex = fullName.indexOf(' ');
        if (spaceIndex > 0) {
          displayName = fullName.substring(0, spaceIndex);
        } else {
          displayName = fullName;
        }
      }

      // 7. Fetch Vehicle ID (Number Plate)
      String vehicleId = "Unknown";
      if (Firebase.getString(firebaseData, basePath + "/vehicle_id")) {
        vehicleId = firebaseData.stringData();
      }

      // 8. Open Gate & Update Detailed OLED
      showTransactionDetails(displayName, vehicleId, vehicleType, dynamicTollFee, newBalance);
      gateServo.write(120); // Open Gate
      
      // 9. Fetch Mobile Number and Send SMS
      if (Firebase.getString(firebaseData, basePath + "/mobile")) {
        String mobileNumber = firebaseData.stringData();
        
        if (!mobileNumber.startsWith("+91")) {
            mobileNumber = "+91" + mobileNumber;
        }

        String smsMessage = "Toll Paid: Rs." + String(dynamicTollFee) + ". New Balance: Rs." + String(newBalance);
        
        sendSMS(mobileNumber, smsMessage); 
      } else {
        Serial.println("No mobile number found for this user.");
        delay(4000); // Standard wait if no SMS is sent
      }
      
      // 10. Close Gate 
      gateServo.write(0); 
      
    } else {
      // Show Low Balance details
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(0, 10);
      display.println("! LOW BALANCE !");
      display.setCursor(0, 30);
      display.println("Req: Rs." + String(dynamicTollFee));
      display.setCursor(0, 45);
      display.println("Bal: Rs." + String(currentBalance));
      display.display();
    }
  } else {
    showOnOLED("Error", "Read Bal Failed");
  }
}