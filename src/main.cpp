#include <Arduino.h>
#include <Bounce2.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include "esp_task_wdt.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <nvs_flash.h>
#include <ESPmDNS.h>
#include "mqtt_config.h"

// ═══════════════════════════════════════════════════════════
// BROCHES
// ═══════════════════════════════════════════════════════════
#define PIN_BTN_APPEL      14
#define PIN_BTN_SANITAIRE  33
#define PIN_LED_R          25
#define PIN_LED_G          26
#define PIN_LED_B          27
#define PIN_BUZZER         32
#define RC522_SS            5
#define RC522_RST          16
#define TOUCHE_URGENCE_1 'A'
#define TOUCHE_URGENCE_2 'D'
#define DUREE_MAINTIEN_URGENCE_MS 1000
bool attenteRelachementCombo = false;
bool     touchesUrgenceEnCours        = false;
uint32_t tempsDebutMaintienUrgence    = 0;
bool     combinaisonUrgenceDeclenchee = false;
uint32_t dernierScanCombinaison       = 0;
#define  INTERVALLE_SCAN_COMBINAISON_MS 50
// ═══════════════════════════════════════════════════════════
// CLAVIER via PCF8574 — Wire direct
// ═══════════════════════════════════════════════════════════
#define PCF8574_ADDR 0x20
#define NO_KEY '\0'
bool premierePublicationFaite = false;
// ── Attente confirmation PIN serveur ──
bool     enAttenteConfirmationPin = false;
uint32_t tempsEnvoiDemandePin = 0;
#define  TIMEOUT_CONFIRMATION_PIN_MS 5000
// Accuse de reception applicatif de /etat : PubSubClient ne publie qu'en
// QoS 0, le serveur renvoie donc la meme "seq" sur .../etat_ack ; sans
// reponse sous TIMEOUT_CONFIRMATION_ETAT_MS, le message est republie.
uint32_t seqEtat                   = 0;
bool     enAttenteConfirmationEtat = false;
uint32_t seqEtatEnAttente          = 0;
uint32_t tempsEnvoiEtat            = 0;
char     dernierPayloadEtat[350];
size_t   dernierPayloadEtatLen     = 0;
#define  TIMEOUT_CONFIRMATION_ETAT_MS 5000
// ═══════════════════════════════════════════════════════════
// PROVISIONING WIFI + IDENTITE CHAMBRE (config en ligne, sans reflash)
// ═══════════════════════════════════════════════════════════
// WiFi et numero de chambre sont stockes en NVS et modifiables via le
// portail, sans reflash. Les topics sont recalcules a partir de
// chambreIdActuelle.
#define NVS_NS_RESEAU "netcfg"
bool   configReseauValide = false;
// Vrai une fois mDNS et heure initialises apres la connexion WiFi.
bool   reseauInitialise   = false;
String ssidConfigure      = "";
String passConfigure      = "";
String chambreIdActuelle  = CHAMBRE_ID;

String topicEtatActuel      = TOPIC_ETAT;
String topicHeartbeatActuel = TOPIC_HEARTBEAT;
String topicStatActuel      = TOPIC_STAT;

// Nom mDNS du serveur central : evite de dependre de son adresse IP.
#define MDNS_NOM_SERVEUR "gemisam-serveur"

WebServer serveurConfig(80);
DNSServer dnsServeurConfig;
const uint8_t KP_ROW_PINS[4] = {7, 6, 5, 4};
const uint8_t KP_COL_PINS[4] = {3, 2, 1, 0};
uint32_t dernierResetRFID = 0;
#define INTERVALLE_RESET_RFID_MS 30000 // toutes les 30s
char kpKeys[4][4] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

uint32_t dernierScanClavier = 0;
#define  INTERVALLE_SCAN_CLAVIER_MS 50
char     derniereTouche     = NO_KEY;
bool     toucheEnCours      = false;

// ═══════════════════════════════════════════════════════════
// LCD 20×4 I2C
// ═══════════════════════════════════════════════════════════
#define LCD_I2C_ADDR 0x27
#define LCD_COLS     20
#define LCD_ROWS     4

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

// ═══════════════════════════════════════════════════════════
// CHRONOMETRE LCD
// ═══════════════════════════════════════════════════════════
uint32_t tempsDebutChrono = 0;
bool     chronoActif      = false;
uint32_t dernierMajChrono = 0;
#define  INTERVALLE_CHRONO_MS 1000
String pinEnAttente = "";
// ═══════════════════════════════════════════════════════════
// DEFILEMENT LCD
// ═══════════════════════════════════════════════════════════
String   scrollTexte   = "";
uint8_t  scrollLigne   = 3;
uint8_t  scrollPos     = 0;
uint32_t dernierScroll = 0;
bool     scrollActif   = false;
#define  INTERVALLE_SCROLL_MS 300

// ═══════════════════════════════════════════════════════════
// TIMEOUT ATTENTE AUTH URGENCE
// ═══════════════════════════════════════════════════════════
uint32_t tempsEntreeAttenteAuth = 0;
#define  DUREE_TIMEOUT_AUTH_MS 30000UL

// ═══════════════════════════════════════════════════════════
// RFID
// ═══════════════════════════════════════════════════════════
MFRC522 rfid(RC522_SS, RC522_RST);

enum RolePersonnel { ROLE_AUCUN, ROLE_INFIRMIER, ROLE_MEDECIN };
struct BadgeAutorise {
  String uid;
  RolePersonnel role;
};
#define MAX_BADGES 50
BadgeAutorise badgesAutorises[MAX_BADGES];
uint8_t nbBadgesAutorises = 0;
// ═══════════════════════════════════════════════════════════
// PIN CODES
// ═══════════════════════════════════════════════════════════
// Les PIN sont individuels et verifies par le serveur.
#define PIN_MAX_LEN    4

// ═══════════════════════════════════════════════════════════
// PWM LED
// ═══════════════════════════════════════════════════════════
#define PWM_FREQ 5000
#define PWM_RES  8
#define CH_R     0
#define CH_G     1
#define CH_B     2

// ═══════════════════════════════════════════════════════════
// WATCHDOG
// ═══════════════════════════════════════════════════════════
#define WDT_TIMEOUT_S 5

// ═══════════════════════════════════════════════════════════
// BOOT-LOOP
// ═══════════════════════════════════════════════════════════
Preferences prefs;
uint8_t bootCount = 0;
#define BOOTLOOP_MAX 3

// ═══════════════════════════════════════════════════════════
// FIFO NVS
// ═══════════════════════════════════════════════════════════
#define FIFO_MAX      50
#define NVS_NS_ALERTE_ACTIVE "alerte_act"

#define NVS_NS_FIFO   "fifo"
#define NVS_KEY_HEAD  "head"
#define NVS_KEY_TAIL  "tail"
#define NVS_KEY_COUNT "count"
struct AlerteNVS {
  char     etat[32];
  char     role[16];
  bool     appel;
  bool     sanitaire;
  bool     urgence;
  uint32_t timestamp;
};

uint8_t fifoHead  = 0;
uint8_t fifoTail  = 0;
uint8_t fifoCount = 0;

// ═══════════════════════════════════════════════════════════
// FSM — 9 ÉTATS
// ═══════════════════════════════════════════════════════════
enum EtatChambre {
  FSM_IDLE,
  FSM_APPEL,
  FSM_SANITAIRE,
  FSM_ATTENTE_AUTH_URGENCE,
  FSM_URGENCE,
  FSM_PRIS_EN_CHARGE_INFIRMIER,
  FSM_PRIS_EN_CHARGE_MEDECIN,
  FSM_CLOTURE,
  FSM_SAFE_MODE
};
void sauvegarderAlerteActive(EtatChambre etat, time_t debutEpoch);
void effacerAlerteActive();
bool chargerAlerteActive(EtatChambre &etatRestaure, time_t &debutRestaure);
void publierEtatAvecEpochOrigine(EtatChambre etat, RolePersonnel role, time_t epochOrigine);

void sauvegarderAlerteActive(EtatChambre etat, time_t debutEpoch) {
  prefs.begin(NVS_NS_ALERTE_ACTIVE, false);
  prefs.putUChar("etat", (uint8_t)etat);
  prefs.putULong("debut", (uint32_t)debutEpoch);
  prefs.end();
}

void effacerAlerteActive() {
  prefs.begin(NVS_NS_ALERTE_ACTIVE, false);
  prefs.clear();
  prefs.end();
}

bool chargerAlerteActive(EtatChambre &etatRestaure, time_t &debutRestaure) {
  prefs.begin(NVS_NS_ALERTE_ACTIVE, true);
  uint8_t etatBrut = prefs.getUChar("etat", 255);
  uint32_t debutBrut = prefs.getULong("debut", 0);
  prefs.end();

  if (etatBrut == 255 || debutBrut == 0) return false;

  etatRestaure = (EtatChambre)etatBrut;
  debutRestaure = (time_t)debutBrut;
  return true;
}
void publierEtatAvecEpochOrigine(EtatChambre etat, RolePersonnel role, time_t epochOrigine);
EtatChambre etatActuel        = FSM_IDLE;
EtatChambre dernierEtatPublie = (EtatChambre)255;
EtatChambre dernierEtatLCD    = (EtatChambre)255;
EtatChambre etatAvantUrgence  = FSM_IDLE;

bool appelActif     = false;
bool sanitaireActif = false;
bool urgenceActif   = false;
String dernierIdentifiantAuth = "";
String methodeAuthentification = "";
RolePersonnel dernierRole          = ROLE_AUCUN;
RolePersonnel roleDeclencheur      = ROLE_AUCUN;
RolePersonnel rolePreneurCharge    = ROLE_AUCUN;
bool          medecinDejaIdentifie = false;

// ═══════════════════════════════════════════════════════════
// STABILISATION BOOT
// ═══════════════════════════════════════════════════════════
bool     bootStabilise      = false;
uint32_t tempsBootStabilise = 0;
#define  DUREE_BOOT_STABILISE_MS 500

// ═══════════════════════════════════════════════════════════
// SAISIE PIN
// ═══════════════════════════════════════════════════════════
String saisiePin        = "";
bool   saisiePinEnCours = false;

// ═══════════════════════════════════════════════════════════
// BIPS NON BLOQUANTS
// ═══════════════════════════════════════════════════════════
enum EtatBip {
  BIP_INACTIF,
  BIP_SIMPLE_ON,
  BIP_DOUBLE_1_ON,
  BIP_DOUBLE_1_OFF,
  BIP_DOUBLE_2_ON,
  BIP_ERREUR_ON,
  BIP_ERREUR_OFF,
  BIP_CONFIRMATION_ON
};

EtatBip  etatBip        = BIP_INACTIF;
uint8_t  compteurBip    = 0;
uint32_t tempsBip       = 0;
uint16_t dureeBipSimple = 0;

// ═══════════════════════════════════════════════════════════
// TIMERS NON BLOQUANTS
// ═══════════════════════════════════════════════════════════
bool     ledJauneAllumee     = false;
uint32_t dernierClignotJaune = 0;
#define  INTERVALLE_CLIGNOT_JAUNE_MS 500

bool     ledOrangeAllumee     = false;
uint32_t dernierClignotOrange = 0;
#define  INTERVALLE_CLIGNOT_ORANGE_MS 400

bool     ledRougeAllumee       = false;
uint32_t dernierClignotUrgence = 0;
#define  INTERVALLE_CLIGNOT_URGENCE_MS 300

bool     ledVerteAllumee    = false;
uint32_t dernierClignotVert = 0;
#define  INTERVALLE_CLIGNOT_VERT_MS 500

bool     buzzerSireneEtat = false;
uint32_t dernierBuzzer    = 0;
#define  INTERVALLE_SIRENE_MS 300

uint32_t tempsEntreeCloture = 0;
#define  DUREE_CLOTURE_MS 2000

uint32_t tempsEntreeIdle    = 0;
bool     idleStableConfirme = false;
#define  DUREE_IDLE_STABLE_MS 10000

bool     msgTransitoireActif = false;
uint32_t tempsMsgTransitoire = 0;
#define  DUREE_MSG_TRANSITOIRE_MS 1500

bool     msgRefusActif = false;
uint32_t tempsMsgRefus = 0;
#define  DUREE_MSG_REFUS_MS 1500

uint32_t tempsDebutAlerte     = 0;
bool     escaladeSonoreActive = false;
#define  DELAI_ESCALADE_MS 300000UL

// ═══════════════════════════════════════════════════════════
// ANTI-REBOND BOUTONS
// ═══════════════════════════════════════════════════════════
Bounce btnAppel     = Bounce();
Bounce btnSanitaire = Bounce();

// ═══════════════════════════════════════════════════════════
// MQTT / WIFI
// ═══════════════════════════════════════════════════════════
WiFiClientSecure espClient;
PubSubClient     mqttClient(espClient);

uint32_t dernierHeartbeat      = 0;
// 10 s : le serveur declare la chambre injoignable apres 25 s sans
// signe de vie, soit 30 s au plus avec son cycle de verification de
// 5 s (DIN VDE 0834 : defaut de liaison signale en 30 s au plus).
#define  INTERVALLE_HEARTBEAT_MS 10000
uint32_t derniereTentativeMQTT = 0;

// ═══════════════════════════════════════════════════════════
// PROTOTYPES
// ═══════════════════════════════════════════════════════════
void setLED(uint8_t r, uint8_t g, uint8_t b);
void declencherBipSimple(uint16_t dureeMs);
void declencherBipDouble();
void declencherBipErreur();
void declencherBipConfirmation();
void gererBipAvance();
void entrerSafeMode();
void transitionVers(EtatChambre nouvelEtat);
void retourEtatPrioritaire();
bool detecterBootLoop();
void gererClignotementJaune();
void gererClignotementOrange();
void gererClignotementUrgence();
void gererClignotementVert();
void gererSireneUrgence();
void gererEscaladeSonore();
void gererMsgTransitoire();
void gererMsgRefus();
void gererChronometre();
void gererScroll();
void gererTimeoutAuth();
void demarrerScroll(String texte, uint8_t ligne);
RolePersonnel lireRFID();
char scannerClavier();
void gererClavierPIN();
void traiterAuthentification(RolePersonnel role);
void connecterWiFi();
void finaliserConnexionWiFi();
void connecterMQTT();
void synchroniserHeure();
void publierEtat(EtatChambre etat, RolePersonnel role);
void publierHeartbeat();
void fifoCharger();
void fifoSauvegarder();
void fifoPush(EtatChambre etat, RolePersonnel role);
void fifoVider();
void fifoPublierEnAttente();
const char* etatToString(EtatChambre e);
const char* roleToString(RolePersonnel r);
void afficherLCD();
void afficherLCDRefus();
void afficherLCDPriseEnCharge(RolePersonnel role);
void afficherChrono(uint8_t ligne);
void lcdEffacerLigne(uint8_t ligne);
void ecrirePCF(uint8_t val);
uint8_t lirePCF();
void chargerConfigReseau();
void sauvegarderConfigReseau(String ssid, String pass, String chambre);
void demarrerProvisioning();
uint8_t verifierToucheMaintenance();
void reinitialisationUsine();
String pageProvisioningHTML();

// ═══════════════════════════════════════════════════════════
// PCF8574 — Wire direct
// ═══════════════════════════════════════════════════════════
void ecrirePCF(uint8_t val) {
  Wire.beginTransmission(PCF8574_ADDR);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t lirePCF() {
  Wire.requestFrom(PCF8574_ADDR, 1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}
// ═══════════════════════════════════════════════════════════
// PROVISIONING WIFI + IDENTITE CHAMBRE
// ═══════════════════════════════════════════════════════════
// Charge la config reseau depuis la NVS. Sans SSID enregistre,
// setup() lance le portail de configuration.
void chargerConfigReseau() {
  prefs.begin(NVS_NS_RESEAU, true);
  ssidConfigure = prefs.getString("ssid", "");
  passConfigure = prefs.getString("pass", "");
  String chambreSauvegardee = prefs.getString("chambre", "");
  prefs.end();

  if (ssidConfigure.length() > 0) {
    configReseauValide = true;
    if (chambreSauvegardee.length() > 0) {
      chambreIdActuelle = chambreSauvegardee;
    }
    Serial.println("[RESEAU] Configuration trouvee en memoire (NVS)");
  } else {
    configReseauValide = false;
    Serial.println("[RESEAU] Aucune configuration enregistree — provisioning requis");
  }

  // Topics recalcules a partir de l'identifiant de chambre configure.
  topicEtatActuel      = "hopital/" + chambreIdActuelle + "/etat";
  topicHeartbeatActuel = "hopital/" + chambreIdActuelle + "/heartbeat";
  topicStatActuel      = "hopital/" + chambreIdActuelle + "/stat";
}

void sauvegarderConfigReseau(String ssid, String pass, String chambre) {
  prefs.begin(NVS_NS_RESEAU, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("chambre", chambre);
  prefs.end();
  Serial.println("[RESEAU] Configuration enregistree — redemarrage...");
}

// Page HTML autonome du portail (aucune ressource externe) : liste des
// reseaux detectes et saisie manuelle en repli.
String pageProvisioningHTML() {
  int nbReseaux = WiFi.scanNetworks();

  String options = "";
  for (int i = 0; i < nbReseaux; i++) {
    options += "<option value=\"" + WiFi.SSID(i) + "\">" +
               WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }

  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>GEMI-SAM-01 — Configuration</title>"
    "<style>"
    "body{font-family:sans-serif;background:#F4F6F9;margin:0;padding:24px;color:#1B3A6B;}"
    "h1{font-size:18px;}"
    "form{background:white;padding:20px;border-radius:10px;max-width:400px;}"
    "label{display:block;margin-top:14px;font-size:13px;color:#546E7A;}"
    "select,input{width:100%;box-sizing:border-box;padding:10px;margin-top:4px;"
    "border:1px solid #D0D8E4;border-radius:6px;font-size:14px;}"
    "button{margin-top:20px;width:100%;padding:12px;background:#1B3A6B;color:white;"
    "border:none;border-radius:6px;font-size:15px;}"
    "</style></head><body>"
    "<h1>GEMI-SAM-01 — Configuration de la chambre</h1>"
    "<form method='POST' action='/save'>"
    "<label>Reseau wifi de l'etablissement</label>"
    "<select name='ssid'>" + options + "</select>"
    "<label>Ou saisir le nom manuellement (si non liste)</label>"
    "<input type='text' name='ssid_manuel' placeholder='Nom du reseau'>"
    "<label>Mot de passe wifi</label>"
    "<input type='password' name='pass'>"
    "<label>Numero de chambre</label>"
    "<input type='text' name='chambre' value='" + chambreIdActuelle + "'>"
    "<button type='submit'>Enregistrer et redemarrer</button>"
    "</form></body></html>";

  return html;
}

// Boucle bloquante : sans WiFi, la chambre ne peut emettre aucune alerte.
// Point d'acces ouvert volontairement : exposition limitee au temps de
// l'installation, aucune donnee patient ne transite.
void demarrerProvisioning() {
  String nomPointAcces = "GEMISAM-CONFIG";
  WiFi.mode(WIFI_AP_STA); // AP + scan simultane des reseaux voisins
  WiFi.softAP(nomPointAcces.c_str());
  IPAddress ipPortail = WiFi.softAPIP(); // 192.168.4.1 par defaut

  Serial.println("[PROVISIONING] Point d'acces temporaire actif : " + nomPointAcces);
  Serial.print("[PROVISIONING] Portail : http://");
  Serial.println(ipPortail);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("CONFIGURATION WIFI ");
  lcd.setCursor(0, 1); lcd.print("Reseau: " + nomPointAcces);
  lcd.setCursor(0, 2); lcd.print("Connectez-vous puis");
  lcd.setCursor(0, 3); lcd.print("ouvrez 192.168.4.1  ");

  // Redirige toutes les requetes DNS vers le portail : declenche la
  // fenetre "Se connecter au reseau" sur les telephones.
  dnsServeurConfig.start(53, "*", ipPortail);

  serveurConfig.on("/", HTTP_GET, []() {
    serveurConfig.send(200, "text/html", pageProvisioningHTML());
  });

  serveurConfig.on("/save", HTTP_POST, []() {
    String ssidManuel = serveurConfig.arg("ssid_manuel");
    String ssid = (ssidManuel.length() > 0) ? ssidManuel : serveurConfig.arg("ssid");
    String pass = serveurConfig.arg("pass");
    String chambre = serveurConfig.arg("chambre");

    if (ssid.length() == 0 || chambre.length() == 0) {
      serveurConfig.send(400, "text/html",
        "<html><body><h3>Nom du reseau et numero de chambre obligatoires — "
        "<a href='/'>retour</a></h3></body></html>");
      return;
    }

    sauvegarderConfigReseau(ssid, pass, chambre);
    serveurConfig.send(200, "text/html",
      "<html><body><h3>Configuration enregistree. "
      "La chambre redemarre et rejoint le reseau...</h3></body></html>");
    delay(1500);
    ESP.restart();
  });

  serveurConfig.onNotFound([ipPortail]() {
    // Tests de connectivite et requetes inconnues renvoyes vers le portail.
    serveurConfig.sendHeader("Location", "http://" + ipPortail.toString() + "/", true);
    serveurConfig.send(302, "text/plain", "");
  });

  serveurConfig.begin();
  Serial.println("[PROVISIONING] Serveur web demarre — en attente de configuration...");

  while (true) {
    esp_task_wdt_reset();
    dnsServeurConfig.processNextRequest();
    serveurConfig.handleClient();
  }
}
// ═══════════════════════════════════════════════════════════
// MAINTENANCE AU DEMARRAGE — portail a la demande / reset usine
// ═══════════════════════════════════════════════════════════
#define DUREE_FENETRE_MAINTENANCE_MS 1500
#define DUREE_MAINTIEN_PORTAIL_MS    3000
#define DUREE_MAINTIEN_RESET_MS      5000

// Retourne 0 = demarrage normal, 1 = portail demande (#), 2 = reset usine (*)
uint8_t verifierToucheMaintenance() {
  lcd.setCursor(0, 2); lcd.print("# 3s : config WiFi  ");
  lcd.setCursor(0, 3); lcd.print("* 5s : reset usine  ");

  // 1) Fenetre d'attente : une touche # ou * est-elle enfoncee ?
  char touche = NO_KEY;
  uint32_t debut = millis();
  while (millis() - debut < DUREE_FENETRE_MAINTENANCE_MS) {
    esp_task_wdt_reset();
    char t = scannerClavier();
    if (t == '#' || t == '*') { touche = t; break; }
    delay(20);
  }
  if (touche == NO_KEY) {
    lcdEffacerLigne(2);
    lcdEffacerLigne(3);
    return 0;
  }

  // 2) La touche doit rester maintenue toute la duree (evite un
  //    declenchement accidentel par un simple appui)
  uint32_t duree = (touche == '#') ? DUREE_MAINTIEN_PORTAIL_MS
                                   : DUREE_MAINTIEN_RESET_MS;
  Serial.printf("[MAINTENANCE] Touche %c detectee — maintenir %lu s\n",
                touche, (unsigned long)(duree / 1000));
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(touche == '#' ? "CONFIG WIFI         " : "RESET USINE         ");
  lcd.setCursor(0, 1); lcd.print("Maintenez la touche ");

  uint32_t debutMaintien = millis();
  uint32_t dernierAffichage = 0;
  while (millis() - debutMaintien < duree) {
    esp_task_wdt_reset();
    if (scannerClavier() != touche) {
      Serial.println("[MAINTENANCE] Touche relachee — annule, demarrage normal");
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("GEMI-SAM-01");
      lcd.setCursor(0, 1); lcd.print("Initialisation...");
      return 0;
    }
    if (millis() - dernierAffichage >= 250) {
      dernierAffichage = millis();
      uint32_t restant = (duree - (millis() - debutMaintien) + 999) / 1000;
      lcd.setCursor(0, 2);
      lcd.printf("Encore %lu s...       ", (unsigned long)restant);
    }
    delay(50);
  }

  // Confirmation sonore + attente du relachement
  digitalWrite(PIN_BUZZER, HIGH); delay(150); digitalWrite(PIN_BUZZER, LOW);
  lcd.setCursor(0, 2); lcd.print("OK - relachez       ");
  uint32_t debutRelache = millis();
  while (scannerClavier() == touche && millis() - debutRelache < 5000) {
    esp_task_wdt_reset();
    delay(50);
  }
  return (touche == '#') ? 1 : 2;
}

// Efface TOUTE la memoire NVS : WiFi, numero de chambre, file d'alertes,
// alerte active, compteur de boot. Au redemarrage, le boitier se comporte
// comme au tout premier allumage (portail GEMISAM-CONFIG).
void reinitialisationUsine() {
  Serial.println("[MAINTENANCE] Reinitialisation usine — effacement de la memoire...");
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("REINITIALISATION    ");
  lcd.setCursor(0, 1); lcd.print("Effacement memoire..");
  setLED(128, 0, 128);

  esp_err_t err = nvs_flash_erase();
  if (err == ESP_OK) err = nvs_flash_init();
  Serial.printf("[MAINTENANCE] Effacement NVS : %s\n",
                err == ESP_OK ? "OK" : esp_err_to_name(err));

  lcd.setCursor(0, 2); lcd.print("Termine.            ");
  lcd.setCursor(0, 3); lcd.print("Redemarrage...      ");
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delay(100);
    digitalWrite(PIN_BUZZER, LOW);  delay(100);
    esp_task_wdt_reset();
  }
  delay(1000);
  ESP.restart();
}

// ═══════════════════════════════════════════════════════════
// COMBINAISON URGENCE DISCRETE — A + D maintenues 1 seconde.
// Remplace entierement le bouton physique d'urgence.
// ═══════════════════════════════════════════════════════════
void verifierCombinaisonUrgence() {
  if (millis() - dernierScanCombinaison < INTERVALLE_SCAN_COMBINAISON_MS) return;
  dernierScanCombinaison = millis();

  bool toucheA_active = false;
  bool toucheD_active = false;

  for (uint8_t r = 0; r < 4; r++) {
    // Nourrit le watchdog a chaque ligne : un bus I2C ralenti ne doit pas
    // provoquer de redemarrage.
    esp_task_wdt_reset();
    ecrirePCF(0xFF);
    uint8_t masque = ~(1 << KP_ROW_PINS[r]);
    ecrirePCF(masque);
    delayMicroseconds(200);
    uint8_t val = lirePCF();

    for (uint8_t c = 0; c < 4; c++) {
      if (!(val & (1 << KP_COL_PINS[c]))) {
        char touche = kpKeys[r][c];
        if (touche == TOUCHE_URGENCE_1) toucheA_active = true;
        if (touche == TOUCHE_URGENCE_2) toucheD_active = true;
      }
    }
  }
  ecrirePCF(0xFF);
  bool combinaisonActive = toucheA_active && toucheD_active;

  if (combinaisonActive) {
    // Combinaison deja traitee : attendre le relachement des deux touches
    // avant tout nouveau declenchement.
    if (attenteRelachementCombo) return;

    if (!touchesUrgenceEnCours) {
      touchesUrgenceEnCours = true;
      tempsDebutMaintienUrgence = millis();
    } else if (millis() - tempsDebutMaintienUrgence >= DUREE_MAINTIEN_URGENCE_MS) {
      combinaisonUrgenceDeclenchee = true;
      touchesUrgenceEnCours = false;
      Serial.println("[URGENCE] Combinaison A+D detectee");
    }
} else {
    touchesUrgenceEnCours = false;
    if (attenteRelachementCombo && !toucheA_active && !toucheD_active) {
        attenteRelachementCombo = false;
        Serial.println("[URGENCE] Touches relachees — clavier PIN reactive");
    }
}
}
// ═══════════════════════════════════════════════════════════
// CHRONOMETRE
// ═══════════════════════════════════════════════════════════
void afficherChrono(uint8_t ligne) {
  if (!chronoActif) return;
  uint32_t elapsed = (millis() - tempsDebutChrono) / 1000;
  uint32_t h = elapsed / 3600;
  uint32_t m = (elapsed % 3600) / 60;
  uint32_t s = elapsed % 60;
  char buf[21];
  snprintf(buf, sizeof(buf), "Duree: %02lu:%02lu:%02lu     ", h, m, s);
  lcd.setCursor(0, ligne);
  lcd.print(buf);
}

void gererChronometre() {
  if (saisiePinEnCours || msgRefusActif) return;
  if (millis() - dernierMajChrono < INTERVALLE_CHRONO_MS) return;
  dernierMajChrono = millis();

  // Chronometre durée alerte
  if (chronoActif && (
      etatActuel == FSM_APPEL              ||
      etatActuel == FSM_SANITAIRE          ||
      etatActuel == FSM_URGENCE)) {
    afficherChrono(2);
  }

  // Compte à rebours timeout auth urgence
  if (etatActuel == FSM_ATTENTE_AUTH_URGENCE) {
    uint32_t ecoule  = millis() - tempsEntreeAttenteAuth;
    uint32_t restant = (ecoule < DUREE_TIMEOUT_AUTH_MS)
                       ? (DUREE_TIMEOUT_AUTH_MS - ecoule) / 1000
                       : 0;
    char buf[21];
    snprintf(buf, sizeof(buf), "Annulation dans: %02lus", restant);
    lcd.setCursor(0, 2);
    lcd.print(buf);
  }
}

// ═══════════════════════════════════════════════════════════
// TIMEOUT AUTH URGENCE
// ═══════════════════════════════════════════════════════════
void gererTimeoutAuth() {
  if (etatActuel != FSM_ATTENTE_AUTH_URGENCE) return;
  if (millis() - tempsEntreeAttenteAuth >= DUREE_TIMEOUT_AUTH_MS) {
    Serial.println("[AUTH] Timeout — fausse alarme → retour état précédent");
    declencherBipErreur();
    urgenceActif = false;
    // Retour à l'état avant l'urgence
    if (etatAvantUrgence == FSM_APPEL) {
      transitionVers(FSM_APPEL);
    } else if (etatAvantUrgence == FSM_SANITAIRE) {
      transitionVers(FSM_SANITAIRE);
    } else if (etatAvantUrgence == FSM_PRIS_EN_CHARGE_INFIRMIER) {
      transitionVers(FSM_PRIS_EN_CHARGE_INFIRMIER);
    } else {
      transitionVers(FSM_IDLE);
    }
  }
}

// ═══════════════════════════════════════════════════════════
// DEFILEMENT LCD
// ═══════════════════════════════════════════════════════════
void demarrerScroll(String texte, uint8_t ligne) {
  scrollTexte   = "                    " + texte + "                    ";
  scrollLigne   = ligne;
  scrollPos     = 0;
  scrollActif   = true;
  dernierScroll = millis();
}

void gererScroll() {
  if (!scrollActif) return;
  if (saisiePinEnCours || msgRefusActif) return;
  if (etatActuel == FSM_ATTENTE_AUTH_URGENCE) return; // ligne 3 utilisée par scroll texte
  if (millis() - dernierScroll < INTERVALLE_SCROLL_MS) return;
  dernierScroll = millis();

  if (scrollTexte.length() < 20) return;

  lcd.setCursor(0, scrollLigne);
  lcd.print(scrollTexte.substring(scrollPos, scrollPos + 20));

  scrollPos++;
  if (scrollPos > (uint8_t)(scrollTexte.length() - 20)) {
    scrollPos = 0;
  }
}
void callbackMQTT(char* topic, byte* payload, unsigned int length);
// ═══════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════
void setup() {
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== GEMI-SAM-01 — Démarrage ===");

  if (detecterBootLoop()) {
    entrerSafeMode();
  }

  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  Serial.println("[WDT] Watchdog activé — timeout 5s");

  ledcSetup(CH_R, PWM_FREQ, PWM_RES);
  ledcSetup(CH_G, PWM_FREQ, PWM_RES);
  ledcSetup(CH_B, PWM_FREQ, PWM_RES);
  ledcAttachPin(PIN_LED_R, CH_R);
  ledcAttachPin(PIN_LED_G, CH_G);
  ledcAttachPin(PIN_LED_B, CH_B);
  setLED(0, 0, 0);

  Wire.begin(21, 22);
  // Timeout I2C : evite un blocage si le PCF8574 ne repond plus.
  Wire.setTimeOut(50);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("GEMI-SAM-01");
  lcd.setCursor(0, 1); lcd.print("Initialisation...");
  Serial.println("[LCD] Initialisé");

  ecrirePCF(0xFF);
  Serial.println(
    "[CLAVIER] PCF8574 initialisé — adresse 0x20");

  btnAppel.attach(PIN_BTN_APPEL,
    INPUT_PULLUP); btnAppel.interval(50);
  btnSanitaire.attach(PIN_BTN_SANITAIRE,
    INPUT_PULLUP); btnSanitaire.interval(50);
  delay(200);
  btnAppel.update();
  btnSanitaire.update();
  Serial.println("[GPIO] Boutons configurés");

  // ── Maintenance au demarrage (touches du clavier) ─────────
  // Fenetre de 1,5 s a chaque demarrage :
  //   # maintenu 3 s -> rouvre le portail (changer de WiFi / chambre)
  //   * maintenu 5 s -> reinitialisation usine (oublie tout, comme neuf)
  // Le portail ne s'ouvre jamais seul sur un echec WiFi : apres une
  // coupure, la box redemarre plus lentement que l'ESP32.
  uint8_t modeMaintenance = verifierToucheMaintenance();
  if (modeMaintenance == 2) {
    reinitialisationUsine(); // ne revient pas (redemarrage)
  }

  fifoCharger();
  Serial.printf("[FIFO] %d alertes en attente\n",
    fifoCount);

  // ── Config reseau (wifi + identite chambre) ────────────────
  // Sans config enregistree, demarrerProvisioning() bloque jusqu'a la
  // configuration puis redemarre l'ESP32.
  chargerConfigReseau();
  if (!configReseauValide || modeMaintenance == 1) {
    if (modeMaintenance == 1) {
      Serial.println("[MAINTENANCE] Portail demande au clavier (#)");
    }
    demarrerProvisioning();
  }

  // WiFi + MQTT avant SPI — évite les conflits
  connecterWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[RESEAU] Demarrage hors ligne — alertes gardees en file NVS. "
                   "Pour changer de WiFi : redemarrer en maintenant # 3 s.");
  }

  mqttClient.setCallback(callbackMQTT);
  connecterMQTT();
  if (mqttClient.connected()) fifoPublierEnAttente();

  // ── RFID — init APRES MQTT ────────────────────────
  // La connexion MQTT peut perturber le bus SPI,
  // on réinitialise le RFID juste après pour garantir
  // un état propre
  pinMode(RC522_RST, OUTPUT);
  digitalWrite(RC522_RST, LOW);
  delay(100);
  digitalWrite(RC522_RST, HIGH);
  delay(200);
  SPI.begin();
  rfid.PCD_Init();
  delay(100);
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);

  // Vérification
  byte version = rfid.PCD_ReadRegister(
    MFRC522::VersionReg);
  Serial.printf("[RFID] Version: 0x%02X %s\n",
    version,
    (version == 0x91 || version == 0x92) ?
      "— OK" : "— ⚠️ Non détecté");

  // ── Reset final RFID ──────────────────────────────
  // Garantit que l'antenne est dans un état propre
  // après toutes les initialisations
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
  Serial.println("[RFID] Antenne réinitialisée — prêt");

  declencherBipSimple(100);
  delay(200);
  declencherBipSimple(100);

EtatChambre etatRestaure;
time_t debutRestaure;
if (chargerAlerteActive(etatRestaure, debutRestaure)) {
  Serial.println("[RECUPERATION] Alerte active trouvee en memoire — restauration");
  etatActuel = etatRestaure;
  publierEtatAvecEpochOrigine(etatActuel, dernierRole, debutRestaure);
  afficherLCD();
} else {
  Serial.println("[FSM] État initial : FSM_IDLE");
  transitionVers(FSM_IDLE);
}

  tempsBootStabilise = millis();
  bootStabilise      = false;
  Serial.println(
    "[BOOT] Attente stabilisation 500ms...");
}
// ═══════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════
void loop() {
  esp_task_wdt_reset();

  // WiFi connecte en arriere-plan apres un demarrage hors ligne :
  // on termine l'initialisation reseau (mDNS + heure) avant MQTT.
  if (!reseauInitialise && WiFi.status() == WL_CONNECTED) {
    Serial.println("[WIFI] Connexion etablie en arriere-plan");
    finaliserConnexionWiFi();
  }
if (enAttenteConfirmationPin &&
    millis() - tempsEnvoiDemandePin >= TIMEOUT_CONFIRMATION_PIN_MS) {
  enAttenteConfirmationPin = false;
  Serial.println("[PIN] Timeout — pas de reponse serveur");
  declencherBipErreur();
  msgRefusActif = true;
  tempsMsgRefus = millis();
  afficherLCDRefus();
}
if (enAttenteConfirmationEtat &&
    millis() - tempsEnvoiEtat >= TIMEOUT_CONFIRMATION_ETAT_MS) {
  if (mqttClient.connected()) {
    mqttClient.publish(topicEtatActuel.c_str(), dernierPayloadEtat, dernierPayloadEtatLen);
    Serial.printf("[ETAT] Pas de confirmation recue (seq=%lu) — republication\n",
                  (unsigned long)seqEtatEnAttente);
  }
  // Relance le delai : republication jusqu'a l'accuse de reception.
  tempsEnvoiEtat = millis();
}
if (etatActuel != FSM_ATTENTE_AUTH_URGENCE &&
    etatActuel != FSM_URGENCE &&
    etatActuel != FSM_SAFE_MODE &&
    !saisiePinEnCours && !msgRefusActif) {
  verifierCombinaisonUrgence();
}
  if (!bootStabilise) {
    if (millis() - tempsBootStabilise >= DUREE_BOOT_STABILISE_MS) {
      bootStabilise = true;
      btnAppel.update();
      btnSanitaire.update();
      Serial.println("[BOOT] Stabilisation OK — boutons actifs");
    } else {
      if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) {
          if (millis() - derniereTentativeMQTT > 10000) {
            derniereTentativeMQTT = millis();
            connecterMQTT();
          }
        } else {
          mqttClient.loop();
        }
      }
      return;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (millis() - derniereTentativeMQTT > 10000) {
        derniereTentativeMQTT = millis();
        connecterMQTT();
        if (mqttClient.connected()) fifoPublierEnAttente();
      }
    } else {
      mqttClient.loop();
    }
  }

  if (millis() - dernierHeartbeat >= INTERVALLE_HEARTBEAT_MS) {
    dernierHeartbeat = millis();
    publierHeartbeat();
  }

if (etatActuel != dernierEtatPublie || !premierePublicationFaite) {
    dernierEtatPublie = etatActuel;
    premierePublicationFaite = true;
    publierEtat(etatActuel, dernierRole);
}

  if (etatActuel != dernierEtatLCD && !msgRefusActif && !saisiePinEnCours) {
    dernierEtatLCD = etatActuel;
    afficherLCD();
  }

  if (etatActuel == FSM_IDLE && !idleStableConfirme &&
      millis() - tempsEntreeIdle >= DUREE_IDLE_STABLE_MS) {
    idleStableConfirme = true;
    prefs.begin("boot", false);
    prefs.putUChar("count", 1);
    prefs.end();
    Serial.println("[BOOT] IDLE stable 10s — compteur remis à 1");
  }

  btnAppel.update();
  btnSanitaire.update();

  gererBipAvance();
  gererClignotementJaune();
  gererClignotementOrange();
  gererClignotementUrgence();
  gererClignotementVert();
  gererSireneUrgence();
  gererEscaladeSonore();
  gererMsgTransitoire();
  gererMsgRefus();
  gererChronometre();
  gererScroll();
  gererTimeoutAuth();

  static uint32_t derniereLectureRFID = 0;
  RolePersonnel roleDetecte = ROLE_AUCUN;

  if (millis() - derniereLectureRFID >= 500 &&
      (!msgRefusActif || etatActuel == FSM_ATTENTE_AUTH_URGENCE) &&
      !msgTransitoireActif) {
    derniereLectureRFID = millis();
    roleDetecte = lireRFID();
  }

  gererClavierPIN();

  switch (etatActuel) {

    case FSM_IDLE:
      if (combinaisonUrgenceDeclenchee) {
  combinaisonUrgenceDeclenchee = false;
  attenteRelachementCombo = true;
        urgenceActif       = true;
        roleDeclencheur    = ROLE_AUCUN;
        etatAvantUrgence   = FSM_IDLE;
        Serial.println("[BTN] Urgence → ATTENTE_AUTH_URGENCE");
        transitionVers(FSM_ATTENTE_AUTH_URGENCE);
      } else if (btnSanitaire.fell()) {
        sanitaireActif = true;
        Serial.println("[BTN] Sanitaire → SANITAIRE");
        transitionVers(FSM_SANITAIRE);
      } else if (btnAppel.fell()) {
        appelActif = true;
        Serial.println("[BTN] Appel → APPEL");
        transitionVers(FSM_APPEL);
      }
      break;

    case FSM_APPEL:
      if (combinaisonUrgenceDeclenchee) {
  combinaisonUrgenceDeclenchee = false;
  attenteRelachementCombo = true;
        urgenceActif     = true;
        etatAvantUrgence = FSM_APPEL;
        Serial.println("[BTN] Urgence depuis APPEL → ATTENTE_AUTH_URGENCE");
        transitionVers(FSM_ATTENTE_AUTH_URGENCE);
      } else if (btnSanitaire.fell()) {
        sanitaireActif = true;
        Serial.println("[BTN] Sanitaire ecrase appel → SANITAIRE");
        transitionVers(FSM_SANITAIRE);
      } else if (roleDetecte != ROLE_AUCUN) {
        traiterAuthentification(roleDetecte);
      }
      break;

    case FSM_SANITAIRE:
      if (combinaisonUrgenceDeclenchee) {
  combinaisonUrgenceDeclenchee = false;
  attenteRelachementCombo = true;
        urgenceActif     = true;
        etatAvantUrgence = FSM_SANITAIRE;
        Serial.println("[BTN] Urgence depuis SANITAIRE → ATTENTE_AUTH_URGENCE");
        transitionVers(FSM_ATTENTE_AUTH_URGENCE);
      } else if (roleDetecte != ROLE_AUCUN) {
        traiterAuthentification(roleDetecte);
      }
      break;

    case FSM_ATTENTE_AUTH_URGENCE:
      if (roleDetecte == ROLE_INFIRMIER) {
        Serial.println("[AUTH] Infirmier validé → URGENCE");
        roleDeclencheur = ROLE_INFIRMIER;
        dernierRole     = ROLE_INFIRMIER;
        transitionVers(FSM_URGENCE);
      } else if (roleDetecte == ROLE_MEDECIN) {
        Serial.println("[AUTH] Médecin → PRIS_EN_CHARGE_MEDECIN direct");
        roleDeclencheur      = ROLE_MEDECIN;
        rolePreneurCharge    = ROLE_MEDECIN;
        dernierRole          = ROLE_MEDECIN;
        medecinDejaIdentifie = true;
        transitionVers(FSM_PRIS_EN_CHARGE_MEDECIN);
      }
      break;

    case FSM_URGENCE:
      if (roleDetecte == ROLE_MEDECIN) {
        Serial.println("[AUTH] Médecin → PRIS_EN_CHARGE_MEDECIN");
        rolePreneurCharge    = ROLE_MEDECIN;
        dernierRole          = ROLE_MEDECIN;
        medecinDejaIdentifie = true;
        transitionVers(FSM_PRIS_EN_CHARGE_MEDECIN);
      } else if (roleDetecte == ROLE_INFIRMIER) {
        Serial.println("[AUTH] Infirmier refusé — médecin requis");
        declencherBipErreur();
        msgRefusActif = true;
        tempsMsgRefus = millis();
        afficherLCDRefus();
      }
      break;

    case FSM_PRIS_EN_CHARGE_INFIRMIER:
      if (combinaisonUrgenceDeclenchee) {
        combinaisonUrgenceDeclenchee = false;
        attenteRelachementCombo = true;

        if (rolePreneurCharge == ROLE_MEDECIN) {
          // Le medecin est deja identifie ET deja en charge de cet
          // appel — pas de nouvelle authentification, pas d'attente :
          // on requalifie simplement l'intervention en cours en
          // urgence. On reste en "prise en charge", seul le type
          // (appel/sanitaire -> urgence) change.
          Serial.println("[URGENCE] Escalade — medecin deja en charge, requalification directe");
          medecinDejaIdentifie = true;
          transitionVers(FSM_PRIS_EN_CHARGE_MEDECIN);
          urgenceActif = true; // transitionVers() le remet a false par defaut
        } else {
          // L'infirmier a deja saisi son code/badge sur l'appel
          // initial — inutile de le refaire authentifier. On passe
          // direct en URGENCE (donc en attente d'un medecin), sans
          // repasser par ATTENTE_AUTH_URGENCE.
          Serial.println("[URGENCE] Escalade — infirmier deja authentifie, attente medecin");
          roleDeclencheur = ROLE_INFIRMIER;
          urgenceActif    = true;
          transitionVers(FSM_URGENCE);
        }
      } else if (!msgTransitoireActif && roleDetecte != ROLE_AUCUN) {
        Serial.println("[AUTH] Clôture → CLOTURE");
        dernierRole = roleDetecte;
        transitionVers(FSM_CLOTURE);
      }
      break;

    case FSM_PRIS_EN_CHARGE_MEDECIN:
      if (combinaisonUrgenceDeclenchee) {
  combinaisonUrgenceDeclenchee = false;
  attenteRelachementCombo = true;
        urgenceActif         = true;
        medecinDejaIdentifie = true;
        etatAvantUrgence     = FSM_PRIS_EN_CHARGE_MEDECIN;
        Serial.println("[BTN] Escalade urgence depuis médecin");
        transitionVers(FSM_ATTENTE_AUTH_URGENCE);
      } else if (!msgTransitoireActif && roleDetecte == ROLE_MEDECIN) {
        Serial.println("[AUTH] Fin intervention médecin → CLOTURE");
        dernierRole = roleDetecte;
        transitionVers(FSM_CLOTURE);
      } else if (!msgTransitoireActif && roleDetecte == ROLE_INFIRMIER && !urgenceActif) {
        Serial.println("[AUTH] Clôture par infirmier → CLOTURE");
        dernierRole = roleDetecte;
        transitionVers(FSM_CLOTURE);
      }
      break;

    case FSM_CLOTURE:
      if (millis() - tempsEntreeCloture >= DUREE_CLOTURE_MS) {
        Serial.println("[FSM] Clôture terminée → retour état prioritaire");
        retourEtatPrioritaire();
      }
      break;

    case FSM_SAFE_MODE:
      break;
  }
}

// ═══════════════════════════════════════════════════════════
// SCAN CLAVIER — Wire direct
// ═══════════════════════════════════════════════════════════
char scannerClavier() {
  for (uint8_t r = 0; r < 4; r++) {
    // Meme raison que dans verifierCombinaisonUrgence().
    esp_task_wdt_reset();
    ecrirePCF(0xFF);
    uint8_t masque = ~(1 << KP_ROW_PINS[r]);
    ecrirePCF(masque);
    delayMicroseconds(200);

    uint8_t val = lirePCF();

    for (uint8_t c = 0; c < 4; c++) {
      if (!(val & (1 << KP_COL_PINS[c]))) {
        ecrirePCF(0xFF);
        return kpKeys[r][c];
      }
    }
  }
  ecrirePCF(0xFF);
  return NO_KEY;
}

// ═══════════════════════════════════════════════════════════
// CLAVIER PIN
// ═══════════════════════════════════════════════════════════
void gererClavierPIN() {
  if (etatActuel == FSM_IDLE || etatActuel == FSM_SAFE_MODE ||
      etatActuel == FSM_CLOTURE) return;

  if (millis() - dernierScanClavier < INTERVALLE_SCAN_CLAVIER_MS) return;
  dernierScanClavier = millis();

  char touche = scannerClavier();

  if (touche != NO_KEY && touche == derniereTouche && toucheEnCours) return;

  if (touche == NO_KEY) {
    toucheEnCours  = false;
    derniereTouche = NO_KEY;
    return;
  }

  toucheEnCours  = true;
  derniereTouche = touche;

  if (touche == '*') {
    saisiePin        = "";
    saisiePinEnCours = false;
    Serial.println("[PIN] Effacé");
    afficherLCD();
    return;
  }

  if (touche == '#') {
    // Le PIN est un secret : jamais affiche en clair sur le port serie.
    Serial.printf("[PIN] Code saisi (%u chiffres)\n", (unsigned)saisiePin.length());
    Serial.println("[PIN] Envoi au serveur pour verification...");

    lcd.clear();
    lcd.setCursor(0, 1); lcd.print("Verification...    ");

    String topicDemande = "hopital/" + chambreIdActuelle + "/auth_pin_request";
    JsonDocument docDemande;
    docDemande["pin"] = saisiePin;
    char bufferDemande[64];
    serializeJson(docDemande, bufferDemande);
    mqttClient.publish(topicDemande.c_str(), bufferDemande);

    enAttenteConfirmationPin = true;
    tempsEnvoiDemandePin = millis();
pinEnAttente = saisiePin;
    saisiePin        = "";
    saisiePinEnCours = false;
    return;
}
// Seuls les chiffres comptent : A/B/C/D servent a d'autres fonctions.
  if (touche < '0' || touche > '9') {
    Serial.printf("[PIN] Touche ignoree (non numerique): %c\n", touche);
    return;
  }

  if (saisiePin.length() < PIN_MAX_LEN) {
    saisiePin += touche;
    saisiePinEnCours = true;
    declencherBipSimple(50);
    Serial.printf("[PIN] Saisie: %u chiffre(s)\n", (unsigned)saisiePin.length());

    scrollActif = false;
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Saisie code PIN:    ");
    lcd.setCursor(0, 1);
    String masque = "";
    for (uint8_t i = 0; i < saisiePin.length(); i++)          masque += "*";
    for (uint8_t i = saisiePin.length(); i < PIN_MAX_LEN; i++) masque += "_";
    lcd.print(masque);
    lcd.setCursor(0, 2); lcd.print("* = effacer         ");
    lcd.setCursor(0, 3); lcd.print("# = valider         ");
  }
}

// ═══════════════════════════════════════════════════════════
// TRAITEMENT AUTHENTIFICATION
// ═══════════════════════════════════════════════════════════
void traiterAuthentification(RolePersonnel role) {
  switch (etatActuel) {

    case FSM_APPEL:
    case FSM_SANITAIRE:
      if (role == ROLE_INFIRMIER || role == ROLE_MEDECIN) {
        dernierRole       = role;
        rolePreneurCharge = role;
        if (role == ROLE_MEDECIN) medecinDejaIdentifie = true;
        transitionVers(FSM_PRIS_EN_CHARGE_INFIRMIER);
      } else {
        declencherBipErreur();
        msgRefusActif = true;
        tempsMsgRefus = millis();
        afficherLCDRefus();
      }
      break;

    case FSM_ATTENTE_AUTH_URGENCE:
      if (role == ROLE_INFIRMIER) {
        roleDeclencheur = ROLE_INFIRMIER;
        dernierRole     = ROLE_INFIRMIER;
        transitionVers(FSM_URGENCE);
      } else if (role == ROLE_MEDECIN) {
        roleDeclencheur      = ROLE_MEDECIN;
        rolePreneurCharge    = ROLE_MEDECIN;
        dernierRole          = ROLE_MEDECIN;
        medecinDejaIdentifie = true;
        transitionVers(FSM_PRIS_EN_CHARGE_MEDECIN);
      } else {
        declencherBipErreur();
        msgRefusActif = true;
        tempsMsgRefus = millis();
        afficherLCDRefus();
      }
      break;

    case FSM_URGENCE:
      if (role == ROLE_MEDECIN) {
        rolePreneurCharge    = ROLE_MEDECIN;
        dernierRole          = ROLE_MEDECIN;
        medecinDejaIdentifie = true;
        transitionVers(FSM_PRIS_EN_CHARGE_MEDECIN);
      } else {
        Serial.println("[AUTH] Infirmier refusé — médecin requis");
        declencherBipErreur();
        msgRefusActif = true;
        tempsMsgRefus = millis();
        afficherLCDRefus();
      }
      break;

    case FSM_PRIS_EN_CHARGE_INFIRMIER:
      if (!msgTransitoireActif &&
          (role == ROLE_INFIRMIER || role == ROLE_MEDECIN)) {
        dernierRole = role;
        transitionVers(FSM_CLOTURE);
      }
      break;

    case FSM_PRIS_EN_CHARGE_MEDECIN:
      if (!msgTransitoireActif && role == ROLE_MEDECIN) {
        dernierRole = role;
        transitionVers(FSM_CLOTURE);
      } else if (!msgTransitoireActif && role == ROLE_INFIRMIER && !urgenceActif) {
        dernierRole = role;
        transitionVers(FSM_CLOTURE);
      }
      break;

    default: break;
  }
}

// ═══════════════════════════════════════════════════════════
// TRANSITIONS FSM
// ═══════════════════════════════════════════════════════════
void transitionVers(EtatChambre nouvelEtat) {
  etatActuel           = nouvelEtat;
  ledJauneAllumee      = false;
  ledOrangeAllumee     = false;
  ledRougeAllumee      = false;
  ledVerteAllumee      = false;
  buzzerSireneEtat     = false;
  msgTransitoireActif  = false;
  msgRefusActif        = false;
  escaladeSonoreActive = false;
  saisiePinEnCours     = false;
  scrollActif          = false;
  digitalWrite(PIN_BUZZER, LOW);
  if (nouvelEtat == FSM_APPEL || nouvelEtat == FSM_SANITAIRE ||
      nouvelEtat == FSM_URGENCE || nouvelEtat == FSM_PRIS_EN_CHARGE_INFIRMIER ||
      nouvelEtat == FSM_PRIS_EN_CHARGE_MEDECIN) {
    time_t maintenant = time(nullptr);
    if (maintenant > 1700000000) {
      sauvegarderAlerteActive(nouvelEtat, maintenant);
    }
  } else if (nouvelEtat == FSM_IDLE) {
    effacerAlerteActive();
  }

  switch (nouvelEtat) {

    case FSM_IDLE:
      appelActif           = false;
      sanitaireActif       = false;
      urgenceActif         = false;
      dernierRole          = ROLE_AUCUN;
      roleDeclencheur      = ROLE_AUCUN;
      rolePreneurCharge    = ROLE_AUCUN;
      medecinDejaIdentifie = false;
      chronoActif          = false;
      etatAvantUrgence     = FSM_IDLE;
      setLED(0, 0, 0);
      tempsEntreeIdle    = millis();
      idleStableConfirme = false;
      Serial.println("[FSM] → IDLE");
      break;

    case FSM_APPEL:
      setLED(255, 0, 0);
      declencherBipSimple(200);
      tempsDebutAlerte     = millis();
      tempsDebutChrono     = millis();
      chronoActif          = true;
      escaladeSonoreActive = false;
      Serial.println("[FSM] → APPEL");
      break;

    case FSM_SANITAIRE:
      declencherBipDouble();
      ledJauneAllumee     = true;
      dernierClignotJaune = millis();
      setLED(255, 100, 0);
      tempsDebutAlerte     = millis();
      tempsDebutChrono     = millis();
      chronoActif          = true;
      escaladeSonoreActive = false;
      Serial.println("[FSM] → SANITAIRE");
      break;

    case FSM_ATTENTE_AUTH_URGENCE:
      ledOrangeAllumee       = true;
      dernierClignotOrange   = millis();
      setLED(255, 15, 0);
      tempsEntreeAttenteAuth = millis();
      chronoActif            = false;
      Serial.println("[FSM] → ATTENTE_AUTH_URGENCE");
      break;

    case FSM_URGENCE:
      ledRougeAllumee       = true;
      dernierClignotUrgence = millis();
      setLED(255, 0, 0);
      buzzerSireneEtat = true;
      dernierBuzzer    = millis();
      digitalWrite(PIN_BUZZER, HIGH);
      if (!chronoActif) {
        tempsDebutChrono = millis();
        chronoActif      = true;
      }
      Serial.println("[FSM] → URGENCE");
      break;

    case FSM_PRIS_EN_CHARGE_INFIRMIER:
      if (sanitaireActif)  sanitaireActif = false;
      else if (appelActif) appelActif     = false;
      chronoActif = false;
      setLED(0, 255, 0);
      declencherBipConfirmation();
      msgTransitoireActif = true;
      tempsMsgTransitoire = millis();
      Serial.println("[FSM] → PRIS_EN_CHARGE_INFIRMIER");
      break;

    case FSM_PRIS_EN_CHARGE_MEDECIN:
      urgenceActif       = false;
      chronoActif        = false;
      ledVerteAllumee    = true;
      dernierClignotVert = millis();
      setLED(0, 255, 0);
      declencherBipConfirmation();
      msgTransitoireActif = true;
      tempsMsgTransitoire = millis();
      Serial.println("[FSM] → PRIS_EN_CHARGE_MEDECIN");
      break;

    case FSM_CLOTURE:
      // Une cloture termine l'episode : tous les drapeaux sont remis a zero,
      // sinon retourEtatPrioritaire() reprendrait un appel deja clos.
      appelActif           = false;
      sanitaireActif       = false;
      urgenceActif         = false;
      medecinDejaIdentifie = false;
      roleDeclencheur      = ROLE_AUCUN;
      rolePreneurCharge    = ROLE_AUCUN;
      chronoActif          = false;
      etatAvantUrgence     = FSM_IDLE;
      setLED(0, 0, 255);
      tempsEntreeCloture = millis();
      declencherBipSimple(300);
      Serial.println("[FSM] → CLOTURE");
      break;

    case FSM_SAFE_MODE:
      appelActif  = sanitaireActif = urgenceActif = false;
      chronoActif = false;
      setLED(128, 0, 128);
      Serial.println("[FSM] → SAFE_MODE");
      break;
  }

  dernierEtatLCD = nouvelEtat;
  afficherLCD();
}

// ═══════════════════════════════════════════════════════════
// RETOUR ÉTAT PRIORITAIRE
// ═══════════════════════════════════════════════════════════
void retourEtatPrioritaire() {
  if (urgenceActif)   { transitionVers(FSM_URGENCE);   return; }
  if (sanitaireActif) { transitionVers(FSM_SANITAIRE); return; }
  if (appelActif)     { transitionVers(FSM_APPEL);     return; }
  transitionVers(FSM_IDLE);
}

// ═══════════════════════════════════════════════════════════
// BIPS NON BLOQUANTS
// ═══════════════════════════════════════════════════════════
void declencherBipSimple(uint16_t dureeMs) {
  if (etatBip != BIP_INACTIF) return;
  dureeBipSimple = dureeMs;
  digitalWrite(PIN_BUZZER, HIGH);
  tempsBip = millis();
  etatBip  = BIP_SIMPLE_ON;
}

void declencherBipDouble() {
  if (etatBip != BIP_INACTIF) return;
  digitalWrite(PIN_BUZZER, HIGH);
  tempsBip = millis();
  etatBip  = BIP_DOUBLE_1_ON;
}

void declencherBipErreur() {
  if (etatBip != BIP_INACTIF) return;
  compteurBip = 0;
  digitalWrite(PIN_BUZZER, HIGH);
  tempsBip = millis();
  etatBip  = BIP_ERREUR_ON;
}

void declencherBipConfirmation() {
  if (etatBip != BIP_INACTIF) return;
  digitalWrite(PIN_BUZZER, HIGH);
  tempsBip = millis();
  etatBip  = BIP_CONFIRMATION_ON;
}

void gererBipAvance() {
  if (etatBip == BIP_INACTIF) return;

  switch (etatBip) {
    case BIP_SIMPLE_ON:
      if (millis() - tempsBip >= dureeBipSimple) {
        digitalWrite(PIN_BUZZER, LOW);
        etatBip = BIP_INACTIF;
      }
      break;
    case BIP_DOUBLE_1_ON:
      if (millis() - tempsBip >= 100) {
        digitalWrite(PIN_BUZZER, LOW);
        tempsBip = millis();
        etatBip  = BIP_DOUBLE_1_OFF;
      }
      break;
    case BIP_DOUBLE_1_OFF:
      if (millis() - tempsBip >= 80) {
        digitalWrite(PIN_BUZZER, HIGH);
        tempsBip = millis();
        etatBip  = BIP_DOUBLE_2_ON;
      }
      break;
    case BIP_DOUBLE_2_ON:
      if (millis() - tempsBip >= 100) {
        digitalWrite(PIN_BUZZER, LOW);
        etatBip = BIP_INACTIF;
      }
      break;
    case BIP_ERREUR_ON:
      if (millis() - tempsBip >= 50) {
        digitalWrite(PIN_BUZZER, LOW);
        tempsBip = millis();
        etatBip  = BIP_ERREUR_OFF;
      }
      break;
    case BIP_ERREUR_OFF:
      if (millis() - tempsBip >= 50) {
        compteurBip++;
        if (compteurBip >= 3) {
          etatBip = BIP_INACTIF;
        } else {
          digitalWrite(PIN_BUZZER, HIGH);
          tempsBip = millis();
          etatBip  = BIP_ERREUR_ON;
        }
      }
      break;
    case BIP_CONFIRMATION_ON:
      if (millis() - tempsBip >= 300) {
        digitalWrite(PIN_BUZZER, LOW);
        etatBip = BIP_INACTIF;
      }
      break;
    default: break;
  }
}

// ═══════════════════════════════════════════════════════════
// RFID — badges synchronises depuis le serveur (badgesAutorises[])
// ═══════════════════════════════════════════════════════════
RolePersonnel lireRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
    return ROLE_AUCUN;

  // Construit l'UID scanne en texte hexadecimal
  String uidScanne = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    char buf[3];
    sprintf(buf, "%02X", rfid.uid.uidByte[i]);
    uidScanne += buf;
  }

  Serial.print("[RFID] UID: ");
  Serial.println(uidScanne);

  for (uint8_t i = 0; i < nbBadgesAutorises; i++) {
    if (badgesAutorises[i].uid == uidScanne) {
      dernierIdentifiantAuth = uidScanne;
      methodeAuthentification = "RFID";

      Serial.print("[RFID] → ");
      Serial.println(badgesAutorises[i].role == ROLE_MEDECIN ? "MÉDECIN" : "INFIRMIER");

      rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
      return badgesAutorises[i].role;
    }
  }

  Serial.println("[RFID] → Non reconnu (absent de la liste synchronisee)");
  rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
  return ROLE_AUCUN;
}
// ═══════════════════════════════════════════════════════════
// AFFICHAGE LCD
// ═══════════════════════════════════════════════════════════
void lcdEffacerLigne(uint8_t ligne) {
  lcd.setCursor(0, ligne);
  lcd.print("                    ");
}

void afficherLCD() {
  lcd.clear();
  scrollActif = false;

  switch (etatActuel) {

    case FSM_IDLE:
      lcd.setCursor(0, 0); lcd.print("CHAMBRE 01          ");
      lcd.setCursor(0, 1); lcd.print("Systeme operationnel");
      lcd.setCursor(0, 2); lcd.print("                    ");
      lcd.setCursor(0, 3); lcd.print("                    ");
      break;

    case FSM_APPEL:
      lcd.setCursor(0, 0); lcd.print("[APPEL] PATIENT     ");
      lcd.setCursor(0, 1); lcd.print("Chambre 01          ");
      lcd.setCursor(0, 2); lcd.print("Duree: 00:00:00     ");
      demarrerScroll("Badger ou saisir PIN pour prise en charge", 3);
      break;

    case FSM_SANITAIRE:
      lcd.setCursor(0, 0); lcd.print("[APPEL] SANITAIRE   ");
      lcd.setCursor(0, 1); lcd.print("Chambre 01          ");
      lcd.setCursor(0, 2); lcd.print("Duree: 00:00:00     ");
      demarrerScroll("Badger ou saisir PIN pour prise en charge", 3);
      break;

    case FSM_ATTENTE_AUTH_URGENCE:
      lcd.setCursor(0, 0); lcd.print("[URGENT] AUTH REQ.  ");
      lcd.setCursor(0, 1); lcd.print("Chambre 01          ");
      lcd.setCursor(0, 2); lcd.print("Annulation dans: 30s");
      demarrerScroll("Presenter badge ou saisir code PIN staff", 3);
      break;

    case FSM_URGENCE:
      lcd.setCursor(0, 0); lcd.print("[CRITIQUE] URGENCE  ");
      lcd.setCursor(0, 1); lcd.print("Chambre 01          ");
      lcd.setCursor(0, 2); lcd.print("Duree: 00:00:00     ");
      demarrerScroll("Badge MEDECIN obligatoire pour prise en charge", 3);
      break;

    case FSM_PRIS_EN_CHARGE_INFIRMIER:
      // Affiche le role reel du preneur en charge (un medecin peut traiter
      // un appel normal).
      afficherLCDPriseEnCharge(rolePreneurCharge);
      break;

    case FSM_PRIS_EN_CHARGE_MEDECIN:
      afficherLCDPriseEnCharge(ROLE_MEDECIN);
      break;

    case FSM_CLOTURE:
      lcd.setCursor(0, 0); lcd.print("INTERVENTION CLOTURE");
      lcd.setCursor(0, 1); lcd.print("Chambre 01          ");
      lcd.setCursor(0, 2); lcd.print("Retour en veille... ");
      lcd.setCursor(0, 3); lcd.print("                    ");
      break;

    case FSM_SAFE_MODE:
      lcd.setCursor(0, 0); lcd.print("[DEFAUT SYSTEME]    ");
      lcd.setCursor(0, 1); lcd.print("Mode securite actif ");
      lcd.setCursor(0, 2); lcd.print("Redemarrage requis  ");
      lcd.setCursor(0, 3); lcd.print("Contacter technicien");
      break;
  }
}

void afficherLCDPriseEnCharge(RolePersonnel role) {
  lcd.setCursor(0, 0); lcd.print("PRISE EN CHARGE     ");
  lcd.setCursor(0, 1);
  if (role == ROLE_MEDECIN) lcd.print("Intervenant: MEDECIN");
  else                      lcd.print("Intervenant: INFIRM.");
  lcd.setCursor(0, 2); lcd.print("                    ");
  demarrerScroll("Badger a nouveau pour cloturer l'intervention", 3);
}

void afficherLCDRefus() {
  scrollActif = false;
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("[ACCES REFUSE]      ");
  lcd.setCursor(0, 1); lcd.print("Badge non autorise  ");
  lcd.setCursor(0, 2); lcd.print("ou PIN incorrect    ");
  lcd.setCursor(0, 3); lcd.print("Reessayer...        ");
}

// ═══════════════════════════════════════════════════════════
// TÂCHES NON BLOQUANTES
// ═══════════════════════════════════════════════════════════
void gererClignotementJaune() {
  if (etatActuel != FSM_SANITAIRE) return;
  if (millis() - dernierClignotJaune >= INTERVALLE_CLIGNOT_JAUNE_MS) {
    dernierClignotJaune = millis();
    ledJauneAllumee     = !ledJauneAllumee;
    setLED(ledJauneAllumee ? 255 : 0, ledJauneAllumee ? 100 : 0, 0);
  }
}

void gererClignotementOrange() {
  if (etatActuel != FSM_ATTENTE_AUTH_URGENCE) return;
  if (millis() - dernierClignotOrange >= INTERVALLE_CLIGNOT_ORANGE_MS) {
    dernierClignotOrange = millis();
    ledOrangeAllumee     = !ledOrangeAllumee;
    setLED(ledOrangeAllumee ? 255 : 0, ledOrangeAllumee ? 15 : 0, 0);
  }
}

void gererClignotementUrgence() {
  if (etatActuel != FSM_URGENCE) return;
  if (millis() - dernierClignotUrgence >= INTERVALLE_CLIGNOT_URGENCE_MS) {
    dernierClignotUrgence = millis();
    ledRougeAllumee       = !ledRougeAllumee;
    setLED(ledRougeAllumee ? 255 : 0, 0, 0);
  }
}

void gererClignotementVert() {
  if (etatActuel != FSM_PRIS_EN_CHARGE_MEDECIN) return;
  if (millis() - dernierClignotVert >= INTERVALLE_CLIGNOT_VERT_MS) {
    dernierClignotVert = millis();
    ledVerteAllumee    = !ledVerteAllumee;
    setLED(0, ledVerteAllumee ? 255 : 0, 0);
  }
}

void gererSireneUrgence() {
  if (etatActuel != FSM_URGENCE) return;
  if (millis() - dernierBuzzer >= INTERVALLE_SIRENE_MS) {
    dernierBuzzer    = millis();
    buzzerSireneEtat = !buzzerSireneEtat;
    digitalWrite(PIN_BUZZER, buzzerSireneEtat ? HIGH : LOW);
  }
}

void gererEscaladeSonore() {
  if (etatActuel != FSM_APPEL && etatActuel != FSM_SANITAIRE) {
    escaladeSonoreActive = false;
    return;
  }
  if (!escaladeSonoreActive &&
      millis() - tempsDebutAlerte >= DELAI_ESCALADE_MS) {
    escaladeSonoreActive = true;
    Serial.println("[ESCALADE] 5 min sans réponse — alarme intensifiée");
  }
  if (escaladeSonoreActive) {
    static uint32_t dernierBipEscalade = 0;
    static bool     bipEscaladeEtat    = false;
    if (millis() - dernierBipEscalade >= 200) {
      dernierBipEscalade = millis();
      bipEscaladeEtat    = !bipEscaladeEtat;
      digitalWrite(PIN_BUZZER, bipEscaladeEtat ? HIGH : LOW);
    }
  }
}

void gererMsgTransitoire() {
  if (!msgTransitoireActif) return;
  if (millis() - tempsMsgTransitoire >= DUREE_MSG_TRANSITOIRE_MS)
    msgTransitoireActif = false;
}

void gererMsgRefus() {
  if (!msgRefusActif) return;
  if (millis() - tempsMsgRefus >= DUREE_MSG_REFUS_MS) {
    msgRefusActif = false;
    afficherLCD();
  }
}

// ═══════════════════════════════════════════════════════════
// LED
// ═══════════════════════════════════════════════════════════
void setLED(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(CH_R, r);
  ledcWrite(CH_G, g);
  ledcWrite(CH_B, b);
}

// ═══════════════════════════════════════════════════════════
// BOOT-LOOP
// ═══════════════════════════════════════════════════════════
bool detecterBootLoop() {
  esp_reset_reason_t raison = esp_reset_reason();
  Serial.printf("[RESET] Raison : %d\n", raison);

  prefs.begin("boot", false);
  if (raison == ESP_RST_POWERON || raison == ESP_RST_EXT) {
    bootCount = 1;
    prefs.putUChar("count", bootCount);
    Serial.println("[BOOT] Démarrage manuel — compteur à 1");
  } else {
    bootCount = prefs.getUChar("count", 0) + 1;
    prefs.putUChar("count", bootCount);
    Serial.printf("[BOOT] Reset auto — %d/%d\n", bootCount, BOOTLOOP_MAX);
  }
  prefs.end();

  if (bootCount > BOOTLOOP_MAX) {
    Serial.println("[BOOT] ⚠️ Seuil dépassé → Safe Mode");
    return true;
  }
  return false;
}

void entrerSafeMode() {
  ledcSetup(CH_R, PWM_FREQ, PWM_RES);
  ledcSetup(CH_G, PWM_FREQ, PWM_RES);
  ledcSetup(CH_B, PWM_FREQ, PWM_RES);
  ledcAttachPin(PIN_LED_R, CH_R);
  ledcAttachPin(PIN_LED_G, CH_G);
  ledcAttachPin(PIN_LED_B, CH_B);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  setLED(128, 0, 128);

  Wire.begin(21, 22);
  Wire.setTimeOut(50); // meme raison qu'en setup() — voir plus haut
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("[DEFAUT SYSTEME]    ");
  lcd.setCursor(0, 1); lcd.print("Mode securite actif ");
  lcd.setCursor(0, 2); lcd.print("Redemarrage requis  ");
  lcd.setCursor(0, 3); lcd.print("Contacter technicien");

  Serial.println("[SAFE MODE] LED mauve — reset requis");
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  while (true) { esp_task_wdt_reset(); delay(1000); }
}

// ═══════════════════════════════════════════════════════════
// FIFO NVS
// ═══════════════════════════════════════════════════════════
void fifoCharger() {
  prefs.begin(NVS_NS_FIFO, true);
  fifoHead  = prefs.getUChar(NVS_KEY_HEAD,  0);
  fifoTail  = prefs.getUChar(NVS_KEY_TAIL,  0);
  fifoCount = prefs.getUChar(NVS_KEY_COUNT, 0);
  prefs.end();
  if (fifoCount > FIFO_MAX) {
    fifoHead = fifoTail = fifoCount = 0;
  }
}

void fifoSauvegarder() {
  prefs.begin(NVS_NS_FIFO, false);
  prefs.putUChar(NVS_KEY_HEAD,  fifoHead);
  prefs.putUChar(NVS_KEY_TAIL,  fifoTail);
  prefs.putUChar(NVS_KEY_COUNT, fifoCount);
  prefs.end();
}

void fifoPush(EtatChambre etat, RolePersonnel role) {
  if (fifoCount >= FIFO_MAX) {
    Serial.println("[FIFO] ⚠️ Pleine — alerte la plus ancienne écrasée");
    fifoHead = (fifoHead + 1) % FIFO_MAX;
    fifoCount--;
  }

  AlerteNVS alerte;
  strlcpy(alerte.etat,   etatToString(etat), sizeof(alerte.etat));
  strlcpy(alerte.role,   roleToString(role), sizeof(alerte.role));
  alerte.appel     = appelActif;
  alerte.sanitaire = sanitaireActif;
  alerte.urgence   = urgenceActif;
  alerte.timestamp = millis();

  char keyAlerte[16];
  snprintf(keyAlerte, sizeof(keyAlerte), "a%d", fifoTail);

  prefs.begin(NVS_NS_FIFO, false);
  prefs.putBytes(keyAlerte, &alerte, sizeof(AlerteNVS));
  prefs.end();

  fifoTail = (fifoTail + 1) % FIFO_MAX;
  fifoCount++;
  fifoSauvegarder();

  Serial.printf("[FIFO] Alerte sauvegardée — %d en attente\n", fifoCount);
}

void fifoVider() {
  prefs.begin(NVS_NS_FIFO, false);
  prefs.clear();
  prefs.end();
  fifoHead = fifoTail = fifoCount = 0;
  Serial.println("[FIFO] Vidée");
}

void fifoPublierEnAttente() {
  if (fifoCount == 0) return;
  if (!mqttClient.connected()) return;

  Serial.printf("[FIFO] Publication de %d alertes en attente...\n", fifoCount);

  while (fifoCount > 0) {
    char keyAlerte[16];
    snprintf(keyAlerte, sizeof(keyAlerte), "a%d", fifoHead);

    AlerteNVS alerte;
    prefs.begin(NVS_NS_FIFO, true);
    prefs.getBytes(keyAlerte, &alerte, sizeof(AlerteNVS));
    prefs.end();

    JsonDocument doc;
    doc["chambre"]    = chambreIdActuelle;
    doc["etat"]       = alerte.etat;
    doc["role"]       = alerte.role;
    doc["appel"]      = alerte.appel;
    doc["sanitaire"]  = alerte.sanitaire;
    doc["urgence"]    = alerte.urgence;
    doc["uptime_ms"]  = alerte.timestamp;
    doc["hors_ligne"] = true;

    char buffer[256];
    size_t n = serializeJson(doc, buffer);
    mqttClient.publish(topicEtatActuel.c_str(), buffer, n);
    Serial.printf("[FIFO] → Publié : %s\n", buffer);

    fifoHead = (fifoHead + 1) % FIFO_MAX;
    fifoCount--;

    esp_task_wdt_reset();
    delay(50);
  }

  fifoVider();
  Serial.println("[FIFO] Toutes les alertes publiées");
}

// ═══════════════════════════════════════════════════════════
// WIFI + MQTT
// ═══════════════════════════════════════════════════════════
void connecterWiFi() {
  WiFi.mode(WIFI_STA); // repasse en mode station normal (au cas ou un provisioning WIFI_AP_STA aurait tourne juste avant)
  // Desactive le modem-sleep : connexion plus stable (alimentation secteur).
  WiFi.setSleep(false);
  Serial.printf("[WIFI] Connexion à %s...\n", ssidConfigure.c_str());
  WiFi.begin(ssidConfigure.c_str(), passConfigure.c_str());
  uint32_t debut = millis();
  while (WiFi.status() != WL_CONNECTED) {
    esp_task_wdt_reset();
    delay(300);
    Serial.print(".");
    if (millis() - debut > 20000) {
      wl_status_t st = WiFi.status();
      Serial.printf("\n[WIFI] Échec (20s) — code %d : ", (int)st);
      if (st == WL_NO_SSID_AVAIL)       Serial.println("reseau introuvable (trop loin, 5 GHz ou nom different)");
      else if (st == WL_CONNECT_FAILED) Serial.println("mot de passe refuse");
      else if (st == WL_DISCONNECTED)   Serial.println("pas de reponse de la box (signal faible ou mot de passe faux)");
      else if (st == WL_IDLE_STATUS)    Serial.println("connexion encore en cours (signal faible) — le boitier continue d'essayer en arriere-plan");
      else                              Serial.println("autre erreur");
      Serial.printf("[WIFI] Nom enregistre : [%s] (%d caracteres)\n",
                    ssidConfigure.c_str(), ssidConfigure.length());
      return;
    }
  }
  finaliserConnexionWiFi();
}

// Finalise l'init reseau une fois le WiFi connecte (setup() ou loop()).
void finaliserConnexionWiFi() {
  Serial.printf("\n[WIFI] Connecté — IP: %s  (signal %d dBm)\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // mDNS pour resoudre gemisam-serveur.local ; en cas d'echec, repli sur
  // MQTT_HOST.
  if (MDNS.begin(("esp32-" + chambreIdActuelle).c_str())) {
    Serial.println("[MDNS] Client mDNS actif");
  } else {
    Serial.println("[MDNS] Erreur d'initialisation — resolution par nom indisponible");
  }

  synchroniserHeure();
  reseauInitialise = true;
}
void synchroniserHeure() {
  Serial.println("[NTP] Synchronisation de l'heure...");
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  time_t maintenant = time(nullptr);
  uint32_t debut = millis();
  while (maintenant < 1700000000) { // grossierement avant nov. 2023
    esp_task_wdt_reset();
    delay(300);
    Serial.print(".");
    maintenant = time(nullptr);
    if (millis() - debut > 15000) {
      Serial.println("\n[NTP] Échec synchronisation (15s) — TLS risque d'échouer");
      return;
    }
  }
  Serial.printf("\n[NTP] Heure synchronisée : %s", ctime(&maintenant));
}
// ═══════════════════════════════════════════════════════════
// CALLBACK MQTT — message recu sur un topic abonne
// ═══════════════════════════════════════════════════════════
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  Serial.print("[MQTT RECU] Topic: ");
  Serial.println(topicStr);

  if (topicStr == "hopital/personnel/badges") {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
      Serial.println("[SYNC] Erreur parsing JSON badges");
      return;
    }

    nbBadgesAutorises = 0;
    for (JsonObject b : doc["badges"].as<JsonArray>()) {
      if (nbBadgesAutorises >= MAX_BADGES) break;
      badgesAutorises[nbBadgesAutorises].uid = b["uid"].as<String>();
      String roleTexte = b["role"].as<String>();
      badgesAutorises[nbBadgesAutorises].role =
        (roleTexte == "MEDECIN") ? ROLE_MEDECIN : ROLE_INFIRMIER;
      nbBadgesAutorises++;
    }
    Serial.printf("[SYNC] %d badges synchronises\n", nbBadgesAutorises);
    return;
  }

  String topicReponsePin = "hopital/" + chambreIdActuelle + "/auth_pin_reponse";
  if (topicStr == topicReponsePin) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
      Serial.println("[PIN] Erreur parsing JSON reponse");
      return;
    }

    String role = doc["role"].as<String>();
    enAttenteConfirmationPin = false;
if (role == "INFIRMIER") {
  dernierIdentifiantAuth = pinEnAttente;
  methodeAuthentification = "PIN";
  traiterAuthentification(ROLE_INFIRMIER);
} else if (role == "MEDECIN") {
  dernierIdentifiantAuth = pinEnAttente;
  methodeAuthentification = "PIN";
  traiterAuthentification(ROLE_MEDECIN);
} else {
  declencherBipErreur();
  msgRefusActif = true;
  tempsMsgRefus = millis();
  afficherLCDRefus();
}

pinEnAttente = "";
    return;
  }

  // ── Accusé de réception applicatif /etat ──
  // Meme "seq" que l'attente en cours : arret de la republication.
  String topicAckEtat = "hopital/" + chambreIdActuelle + "/etat_ack";
  if (topicStr == topicAckEtat) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
      Serial.println("[ETAT] Erreur parsing JSON accuse reception");
      return;
    }
    uint32_t seqConfirme = doc["seq"] | 0;
    if (enAttenteConfirmationEtat && seqConfirme == seqEtatEnAttente) {
      enAttenteConfirmationEtat = false;
      Serial.printf("[ETAT] Confirmation recue (seq=%lu)\n", (unsigned long)seqConfirme);
    }
    return;
  }
}
void connecterMQTT() {
    if (WiFi.status() != WL_CONNECTED) return;

    // TLS sans vérification CA — réseau local fermé
    espClient.setInsecure();

    // Recherche du serveur par mDNS ; repli sur MQTT_HOST si introuvable.
    String hoteMqtt = MQTT_HOST;
    IPAddress ipServeur = MDNS.queryHost(MDNS_NOM_SERVEUR, 3000);
    if (ipServeur != IPAddress(0, 0, 0, 0)) {
      hoteMqtt = ipServeur.toString();
      Serial.printf("[MDNS] %s.local trouve : %s\n", MDNS_NOM_SERVEUR, hoteMqtt.c_str());
    } else {
      Serial.printf("[MDNS] %s.local introuvable — repli sur %s\n", MDNS_NOM_SERVEUR, MQTT_HOST);
    }

    mqttClient.setServer(hoteMqtt.c_str(), MQTT_PORT);
    mqttClient.setBufferSize(512);
    // Timeout de 2 s (15 s par defaut) : reste sous le watchdog de 5 s.
    mqttClient.setSocketTimeout(2);

    Serial.printf("[MQTT] Connexion à %s:%d...\n",
        hoteMqtt.c_str(), MQTT_PORT);
    String clientId = "ESP32_" + chambreIdActuelle;

    esp_task_wdt_delete(NULL);
    bool ok = mqttClient.connect(
        clientId.c_str(),
        MQTT_USER, MQTT_PASSWORD,
        topicStatActuel.c_str(), 1, true, "OFFLINE"
    );
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();

    if (ok) {
        mqttClient.publish(topicStatActuel.c_str(), "ONLINE", true);
        mqttClient.subscribe("hopital/personnel/badges");
        String topicReponsePin = "hopital/" + chambreIdActuelle + "/auth_pin_reponse";
        mqttClient.subscribe(topicReponsePin.c_str());
        String topicAckEtat = "hopital/" + chambreIdActuelle + "/etat_ack";
        mqttClient.subscribe(topicAckEtat.c_str());
        Serial.println("[MQTT] Connecté ✓ — ONLINE publié");
    } else {
        Serial.printf("[MQTT] Échec (rc=%d)\n",
            mqttClient.state());
    }
}
// ═══════════════════════════════════════════════════════════
// PUBLICATION MQTT
// ═══════════════════════════════════════════════════════════
void publierEtat(EtatChambre etat, RolePersonnel role) {
  JsonDocument doc;
  doc["chambre"]             = chambreIdActuelle;
  doc["etat"]                = etatToString(etat);
  doc["role"]                = roleToString(role);
  doc["role_declencheur"]    = roleToString(roleDeclencheur);
  doc["role_preneur_charge"] = roleToString(rolePreneurCharge);
  doc["identifiant_auth"]    = dernierIdentifiantAuth;
  doc["methode_auth"]        = methodeAuthentification;
  doc["appel"]               = appelActif;
  doc["sanitaire"]           = sanitaireActif;
  doc["urgence"]             = urgenceActif;
  doc["uptime_ms"]           = millis();
  doc["hors_ligne"]          = false;
  doc["seq"]                 = ++seqEtat;

  // Heure reelle de debut d'alerte : aligne les chronos LCD et interface.
  EtatChambre etatRestaureTmp;
  time_t debutTmp;
  if (chargerAlerteActive(etatRestaureTmp, debutTmp)) {
    doc["epoch_debut_origine"] = (unsigned long)debutTmp;
  }

  char buffer[350];
  size_t n = serializeJson(doc, buffer);

  if (mqttClient.connected()) {
    mqttClient.publish(topicEtatActuel.c_str(), buffer, n);
    Serial.printf("[MQTT] → %s : %s\n", topicEtatActuel.c_str(), buffer);
    dernierIdentifiantAuth = "";
    methodeAuthentification = "";

    // Arme l'attente d'accuse de reception (voir loop() et callbackMQTT()).
    memcpy(dernierPayloadEtat, buffer, n);
    dernierPayloadEtatLen     = n;
    seqEtatEnAttente          = seqEtat;
    enAttenteConfirmationEtat = true;
    tempsEnvoiEtat            = millis();
  } else {
    fifoPush(etat, role);
  }
}
void publierEtatAvecEpochOrigine(EtatChambre etat, RolePersonnel role, time_t epochOrigine) {
  JsonDocument doc;
  doc["chambre"]             = chambreIdActuelle;
  doc["etat"]                = etatToString(etat);
  doc["role"]                = roleToString(role);
  doc["role_declencheur"]    = roleToString(roleDeclencheur);
  doc["role_preneur_charge"] = roleToString(rolePreneurCharge);
  doc["identifiant_auth"]    = "";
  doc["methode_auth"]        = "";
  doc["appel"]               = (etat == FSM_APPEL);
  doc["sanitaire"]           = (etat == FSM_SANITAIRE);
  doc["urgence"]             = (etat == FSM_URGENCE);
  doc["uptime_ms"]           = millis();
  doc["hors_ligne"]          = false;
  doc["epoch_debut_origine"] = (unsigned long)epochOrigine;
  doc["seq"]                 = ++seqEtat;

  char buffer[350];
  size_t n = serializeJson(doc, buffer);

  if (mqttClient.connected()) {
    mqttClient.publish(topicEtatActuel.c_str(), buffer, n);
    Serial.printf("[MQTT] → %s : %s\n", topicEtatActuel.c_str(), buffer);

    memcpy(dernierPayloadEtat, buffer, n);
    dernierPayloadEtatLen     = n;
    seqEtatEnAttente          = seqEtat;
    enAttenteConfirmationEtat = true;
    tempsEnvoiEtat            = millis();
  } else {
    fifoPush(etat, role);
  }
}
void publierHeartbeat() {
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  doc["chambre"]    = chambreIdActuelle;
  doc["uptime_ms"]  = millis();
  doc["rssi"]       = WiFi.RSSI();
  doc["fifo_count"] = fifoCount;

  char buffer[128];
  size_t n = serializeJson(doc, buffer);
  mqttClient.publish(topicHeartbeatActuel.c_str(), buffer, n);
  Serial.printf("[MQTT] Heartbeat: %s\n", buffer);
}

// ═══════════════════════════════════════════════════════════
// UTILITAIRES
// ═══════════════════════════════════════════════════════════
const char* etatToString(EtatChambre e) {
  switch (e) {
    case FSM_IDLE:                     return "IDLE";
    case FSM_APPEL:                    return "APPEL";
    case FSM_SANITAIRE:                return "SANITAIRE";
    case FSM_ATTENTE_AUTH_URGENCE:     return "ATTENTE_AUTH_URGENCE";
    case FSM_URGENCE:                  return "URGENCE";
    case FSM_PRIS_EN_CHARGE_INFIRMIER: return "PRIS_EN_CHARGE_INFIRMIER";
    case FSM_PRIS_EN_CHARGE_MEDECIN:   return "PRIS_EN_CHARGE_MEDECIN";
    case FSM_CLOTURE:                  return "CLOTURE";
    case FSM_SAFE_MODE:                return "SAFE_MODE";
    default:                           return "INCONNU";
  }
}

const char* roleToString(RolePersonnel r) {
  switch (r) {
    case ROLE_INFIRMIER: return "INFIRMIER";
    case ROLE_MEDECIN:   return "MEDECIN";
    default:             return "AUCUN";
  }
}