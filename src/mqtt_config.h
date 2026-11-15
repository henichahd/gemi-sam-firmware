#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

// Identifiants et adresse du broker : voir secrets.h (non versionne).
// Copier secrets.example.h en secrets.h puis le remplir.
#include "secrets.h"

// Le WiFi se configure depuis un telephone via le portail
// GEMISAM-CONFIG (http://192.168.4.1).
// Changer de WiFi : redemarrer le boitier en maintenant # 3 s.
// Tout effacer : redemarrer en maintenant * 5 s.

#define MQTT_PORT       8883

// Valeurs par defaut avant configuration via le portail.
#define CHAMBRE_ID      "001"
#define TOPIC_ETAT      "hopital/001/etat"
#define TOPIC_HEARTBEAT "hopital/001/heartbeat"
#define TOPIC_STAT      "hopital/001/stat"

#endif
