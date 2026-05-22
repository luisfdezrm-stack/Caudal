/*
Lectura de pulsos de caudalímetro. 476 pulsos por litro. 
Envía datos a
https://script.google.com/macros/s/AKfycbyNaTGCCJc9_HNSx5ZjbwB4H5bFlEE1KT-PmUTpUU1SNQbjhfcX-gKEQQUwgCmNliOq/exec
Versión del firmware: 0.6 
Cambios en esta versión:
cambi en la gestión del string post a google sheets.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <WiFiManager.h>
#include <HTTPUpdate.h>
#include <Wire.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>  //para almacenar vbles en ESP32 memoria no volátil

WiFiMulti wifiMulti; 

const float VERSION_ACTUAL = 0.6;

const int ledPin = 2; // para el ESP32
char scriptURL[256] = "";
bool shouldSaveConfig = false;
bool primerLoop = true;
unsigned long previousMillis = 0;
const unsigned long interval = 120000; // aprox 2 minutos
const unsigned long UPDATE_INTERVAL = 3600000;// aprox 1 hora

const char* URL_VERSION = "https://raw.githubusercontent.com/luisfdezrm-stack/Caudal/main/version_actual";
const char* URL_BINARIO = "https://raw.githubusercontent.com/luisfdezrm-stack/Caudal/main/firmware.bin";

const byte PIN_SENSOR = 18; 
const float PULSOS_POR_LITRO = 476.0; 

volatile unsigned long contadorPulsos = 0; // Variables volátiles para la interrupción
unsigned long tiempoAnterior = 0;
const unsigned long INTERVALO = 1000; 
unsigned long totalPulsosAcumulados = 0; 
float caudal_LPM = 0.0;
float volumenTotal_Litros = 0.0;
float  last_caudal = 0.0;
float  last_volumen = 0.0;

const float UMBRAL_CAUDAL = 0.05;   // Variación de 0.5 L/min
const float UMBRAL_VOLUMEN = 0.01;  // Variación de 100 ml

void IRAM_ATTR contarPulso() {contadorPulsos++;}
void configurarSerial();
void configurarSistemaArchivos();
void configurarWiFiDesdeFS();
void gestionarConexionWifi();
void configurarOTA();
void inicializarHardware();
void checkParaActualizar();
void ejecutarCicloLectura();
bool haCambiadoElDato();
void gestionarEnvioDatos();
void enviarAGoogleSheets();
void blinkLED();


// ==========================================
//                SET-UP
// ==========================================
void setup() {
  configurarSerial();
  configurarSistemaArchivos();
  configurarWiFiDesdeFS();
  if (wifiMulti.run() != WL_CONNECTED) {gestionarConexionWifi(); }
  configurarOTA();
  inicializarHardware();
//  LittleFS.remove("/config_url.txt"); //limpia la URL que está guardada en la memoria
  checkParaActualizar();
  Serial.println("\n>>> Sistema Inicializado Correctamente (SETUP)");
  Serial.printf("\n>>> fin del setup, iniciando programa con versión %.2f\n", VERSION_ACTUAL);
}



// ==========================================
//                LOOP
// ==========================================
void loop() {
  if (primerLoop) { Serial.printf("\n>>> Inicio del loop (versión %.2f)\n", VERSION_ACTUAL); primerLoop = false;}
  unsigned long tiempoActual = millis();

  // CÁLCULO SEGUNDO A SEGUNDO EN SEGUNDO PLANO
if (tiempoActual - tiempoAnterior >= INTERVALO) {
    noInterrupts();   // Lectura de los pulsos del último segundo
    unsigned long pulsosCopiados = contadorPulsos;
    contadorPulsos = 0; 
    interrupts();
    tiempoAnterior = tiempoActual;
    totalPulsosAcumulados += pulsosCopiados; // Integración: sumamos los pulsos de este segundo al total histórico
    caudal_LPM = (pulsosCopiados / PULSOS_POR_LITRO) * 60.0;     // Cálculo de Caudal Instantáneo
    volumenTotal_Litros = totalPulsosAcumulados / PULSOS_POR_LITRO; // Cálculo de Volumen Total (Integrado)
    
    // Mostrar resultados por consola
    Serial.print("Caudal: ");
    Serial.print(caudal_LPM, 2);
    Serial.print(" L/min | Volumen Total: ");
    Serial.print(volumenTotal_Litros, 3); // 3 decimales para ver el goteo fino
    Serial.println(" L");
  }

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
//    ejecutarCicloLectura();
    if (haCambiadoElDato()) {
      gestionarEnvioDatos();
      // Guardamos el estado actual como último enviado
    last_caudal = caudal_LPM;
    last_volumen = volumenTotal_Litros;
    } else {Serial.println(">>> Datos estables. No se requiere envío."); }
        checkParaActualizar();
  }
  ArduinoOTA.handle();      // Mantiene activa la actualización inalámbrica
}

// ==========================================
//               FUNCIONES
// ==========================================

void inicializarHardware() {
   pinMode(PIN_SENSOR, INPUT_PULLUP);
   pinMode(ledPin, OUTPUT); // Configurar pin del LED
  attachInterrupt(digitalPinToInterrupt(PIN_SENSOR), contarPulso, FALLING);
  Serial.println("Sensor YF-B10 con cálculo de Volumen Inicializado.");
  Serial.println("proceso: inicializarHardware, ejecutado");
}


// void ejecutarCicloLectura() { }



bool haCambiadoElDato() {
  bool cambioCaudal = abs(caudal_LPM - last_caudal) >= UMBRAL_CAUDAL;
  bool cambioVolumen = abs(volumenTotal_Litros - last_volumen) >= UMBRAL_VOLUMEN;
  return (cambioCaudal || cambioVolumen);}

void saveConfigCallback() {shouldSaveConfig = true;} // Callback de WiFiManager para avisar que debe guardar datos

void configurarSerial() {Serial.begin(115200); delay(100); }

void checkParaActualizar() {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.setUserAgent("ESP32-S2-Mini");
  Serial.println("Comprobando actualizaciones en GitHub...");
  Serial.printf("\n>>> Versión actual> %.2f \n", VERSION_ACTUAL);
  http.begin(client, URL_VERSION); 
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    float version_remota = payload.toFloat();
    // 2. Comparar: ¿Es la versión de la web mayor que la mía?
    if (version_remota > VERSION_ACTUAL) {
      Serial.printf("Nueva versión detectada: %.2f. Actualizando...\n", version_remota);
     // Configuramos el timeout para descargas pesadas
      httpUpdate.setLedPin(ledPin, LOW); // Opcional: parpadea el LED durante la descarga
        // 3. Ejecutar la descarga del binario solo si la versión es superior
      t_httpUpdate_return ret = httpUpdate.update(client, URL_BINARIO);
    switch (ret) {
    case HTTP_UPDATE_FAILED: Serial.printf("Error: %s\n", httpUpdate.getLastErrorString().c_str()); break;
    case HTTP_UPDATE_OK: Serial.println("Actualización terminada! Nueva versión zzzzzzzzzzzzzzzzzzzzzzzz descargado desde github zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"); break;
  }
    } else {Serial.println("El firmware está al día."); }
  }
  else { Serial.printf("Error al conectar con GitHub para verificar versión. Código HTTP: %d\n", httpCode);  }
  http.end();  
}

void configurarSistemaArchivos() {
  if (!LittleFS.begin(false)) { Serial.println("Error montando LittleFS. Intentando formatear...");
    if (!LittleFS.begin(true)) {Serial.println("Error crítico: no se pudo montar ni formatear LittleFS."); return; }
    Serial.println("LittleFS formateado correctamente.");  }
  if (LittleFS.exists("/config_url.txt")) {  File f = LittleFS.open("/config_url.txt", "r");
    if (f) {
      String storedURL = f.readString();
      storedURL.trim();  // elimina saltos de línea y espacios
      strncpy(scriptURL, storedURL.c_str(), sizeof(scriptURL) - 1); //Copia segura al buffer
      scriptURL[sizeof(scriptURL) - 1] = '\0';
      Serial.println("URL cargada desde LittleFS:");
      Serial.println(scriptURL);
      f.close();
    } else {Serial.println("Error abriendo /config_url.txt"); }
  } else {Serial.println("No existe /config_url.txt, usando URL por defecto."); }
}

void gestionarConexionWifi() {
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);
  // Añadimos campo para la URL de Google Script
  WiFiManagerParameter custom_script_url("script", "Google Script URL", scriptURL, 150);
  wm.addParameter(&custom_script_url);
  wm.setConfigPortalTimeout(180); 
  // Intentar conectar (Si falla crea AP "ESP32_Sensor_Config")
  if (!wm.autoConnect("ESP32_Sensor_Config")) {Serial.println("No hay credenciales guardadas o fallo. Abrimos portal como Punto de acceso, AP: ESP32_Sensor_Config......");
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {delay(500);Serial.print(".");retries++; }
 if (WiFi.status() != WL_CONNECTED) { Serial.println("\nFallback fallido. Abriendo portal..."); wm.startConfigPortal("ESP32_Sensor_Config"); }
  }
  if (shouldSaveConfig) {    // Guardar la URL si se cambió en el portal
strncpy(scriptURL, custom_script_url.getValue(), sizeof(scriptURL) - 1);
scriptURL[sizeof(scriptURL) - 1] = '\0';
    File f = LittleFS.open("/config_url.txt", "w");
    if (f) {
      f.print(scriptURL);
      f.close();
      Serial.println("Nueva URL guardada en LittleFS.");
      shouldSaveConfig = false; 
    }
  }
 Serial.print("IP: "); Serial.println(WiFi.localIP());
}

void configurarWiFiDesdeFS() {
  if (LittleFS.exists("/wifi_creds.txt")) {
    File f = LittleFS.open("/wifi_creds.txt", "r");
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      int sep = line.indexOf(';');
      if (sep != -1) {
        String ssid = line.substring(0, sep);
        String pass = line.substring(sep + 1);
        wifiMulti.addAP(ssid.c_str(), pass.c_str());
        Serial.printf("Añadida red: %s\n", ssid.c_str());
      }
    }
    f.close();
  }
  Serial.println("Conectando WiFi...");
  if (wifiMulti.run() == WL_CONNECTED) {Serial.print("Conectado a: "); Serial.println(WiFi.SSID()); }
}

void configurarOTA() { ArduinoOTA.setHostname("esp32-sensor-nivel"); ArduinoOTA.begin();}
 

void gestionarEnvioDatos() {
  if (WiFi.status() == WL_CONNECTED) { enviarAGoogleSheets(); } 
    else { Serial.println("WiFi desconectado. Reintentando..."); WiFi.reconnect(); } }

void enviarAGoogleSheets() {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, scriptURL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String postData = "caudal=" + String(caudal_LPM, 2) +
                    "&volumen=" + String(volumenTotal_Litros, 3) +
                    "&version=" + String(VERSION_ACTUAL, 2);
    Serial.print(">>> Enviando a Google: ");
    Serial.print("postData = ");
    Serial.println(postData);
  int httpCode = http.POST((uint8_t*)postData.c_str(), postData.length());
  // int httpCode = http.POST(postData);
  if (httpCode > 0) {
    Serial.println(">>> Datos enviados con éxito.");
    Serial.print("postData = ");
    Serial.println(postData);
  } else {Serial.printf("Error de envío HTTP: %s\n", http.errorToString(httpCode).c_str()); }
  http.end();
}


// void blinkLED() {digitalWrite(ledPin, HIGH); delay(100); digitalWrite(ledPin, LOW); }
