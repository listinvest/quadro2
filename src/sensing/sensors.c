/*
 * File: sensors.c
 * ----------------------------
 * Author: Niklaus Leuenberger
 * Date:   2019-11-28
 * ----------------------------
 * Implementiert die einzelnen Sensoren und erweitert ggf. deren Funktionen
 *    BNO080:     - Beschleunigung
 *                - Rotation
 *                - Magnetisch Nord
 *                > lineare Beschleunigung
 *                > Orientierung
 *                > uvm.
 *    HC-SR04:    - Abstand zum Boden per Ultraschall
 *    BN-880Q:    - GPS, GLONASS, BeiDou, SBAS, Galileo
 * 
 * Alle Sensoren sind in ihrem eigenen Task implementiert.
 * Neue Sensordaten werden dem Sensor-Task mitgeteilt welcher den absoluten Status des Systems
 * aktualisiert, Sensor-Fusion betreibt und an registrierte Handler weiterleitet.
 * 
 * Systemstatus: (alle Werte im ENU Koordinatensystem)
 *  - Orientierung
 *  - geschätzte Position
 *  - geschätzte Geschwindigkeit
 *
 * Höhe z:
 *  - lineare z Beschleunigung (Worldframe) doppelt integriert über Zeit
 *  - Ultraschall
 *  - GPS Höhe über Meer tariert auf Startposition
 *  - Drucksensor tariert auf Startposition
 * 
 * Position y:
 *  - lineare y Beschleunigung (Worldframe) doppelt integriert über Zeit
 *  - GPS y-Positionskomponente
 * 
 * Position x:
 *  - lineare x Beschleunigung (Worldframe) doppelt integriert über Zeit
 *  - GPS x-Positionskomponente
 */


/** Externe Abhängigkeiten **/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include "eekf.h"


/** Interne Abhängigkeiten **/

#include "resources.h"
#include "i2c.h"
#include "bno.h"
#include "ultrasonic.h"
#include "gps.h"
#include "sensor_types.h"
#include "sensors.h"


/** Variablendeklaration **/

struct sensors_t {
    int64_t timeouts[SENSORS_MAX];

    struct { // Fusion der Z Achse (Altitude)
        eekf_context ekf;
        eekf_mat x, P, z;
        int64_t lastTimestamp;
    } Z;

    struct { // Fusion der Y Achse (Latitude)
        eekf_context ekf;
        eekf_mat x, P, z;
        int64_t lastTimestamp;
    } Y;

    struct { // Fusion der X Achse (Longitude)
        eekf_context ekf;
        eekf_mat x, P, z;
        int64_t lastTimestamp;
    } X;
};
static struct sensors_t sensors;


/** Private Functions **/

/*
 * Function: sensors_task
 * ----------------------------
 * Haupttask. Verwaltet alle Sensorikdaten.
 *
 * void* arg: Dummy für FreeRTOS
 */
void sensors_task(void* arg);

// ToDo
static void sensors_fuseZ_reset();
static void sensors_fuseZ(enum sensors_input_type_t type, float z, int64_t timestamp);
eekf_return sensors_fuseZ_transition(eekf_mat* xp, eekf_mat* Jf, eekf_mat const *x, eekf_mat const *u, void* userData);
eekf_return sensors_fuseZ_measurement(eekf_mat* zp, eekf_mat* Jh, eekf_mat const *x, void* userData);
static void sensors_fuseY_reset();
static void sensors_fuseY(enum sensors_input_type_t type, float y, int64_t timestamp);
eekf_return sensors_fuseY_transition(eekf_mat* xp, eekf_mat* Jf, eekf_mat const *x, eekf_mat const *u, void* userData);
eekf_return sensors_fuseY_measurement(eekf_mat* zp, eekf_mat* Jh, eekf_mat const *x, void* userData);
static void sensors_fuseX_reset();
static void sensors_fuseX(enum sensors_input_type_t type, float x, int64_t timestamp);
eekf_return sensors_fuseX_transition(eekf_mat* xp, eekf_mat* Jf, eekf_mat const *x, eekf_mat const *u, void* userData);
eekf_return sensors_fuseX_measurement(eekf_mat* zp, eekf_mat* Jh, eekf_mat const *x, void* userData);


/** Implementierung **/

bool sensors_init(gpio_num_t scl, gpio_num_t sda,                                   // I2C
                  uint8_t bnoAddr, gpio_num_t bnoInterrupt, gpio_num_t bnoReset,    // BNO080
                  gpio_num_t ultTrigger, gpio_num_t ultEcho,                        // Ultraschall
                  gpio_num_t gpsRxPin, gpio_num_t gpsTxPin) {                       // GPS
    // Input-Queue erstellen
    xSensors_input = xQueueCreate(16, sizeof(struct sensors_input_t));
    // I2C initialisieren
    bool ret = false;
    ESP_LOGD("sensors", "I2C init");
    ret = i2c_init(scl, sda);
    ESP_LOGD("sensors", "I2C %s", ret ? "error" : "ok");
    // BNO initialisieren + Reports für Beschleunigung, Orientierung und Druck aktivieren
    ESP_LOGD("sensors", "BNO init");
    ret |= bno_init(bnoAddr, bnoInterrupt, bnoReset);
    ESP_LOGD("sensors", "BNO %s", ret ? "error" : "ok");
    // Ultraschall initialisieren
    ESP_LOGD("sensors", "ULT init");
    ret |= ult_init(ultTrigger, ultEcho);
    ESP_LOGD("sensors", "ULT %s", ret ? "error" : "ok");
    // GPS initialisieren
    ESP_LOGD("sensors", "GPS init");
    ret |= gps_init(gpsRxPin, gpsTxPin);
    ESP_LOGD("sensors", "GPS %s", ret ? "error" : "ok");
    // Kalman Filter Z initialisieren
    EEKF_CALLOC_MATRIX(sensors.Z.x, 2, 1); // 2 States: Position, Geschwindigkeit
    EEKF_CALLOC_MATRIX(sensors.Z.P, 2, 2);
    EEKF_CALLOC_MATRIX(sensors.Z.z, 3, 1); // 3 Messungen: Ultraschall, Barometer, GPS
    sensors_fuseZ_reset();
    eekf_init(&sensors.Z.ekf, &sensors.Z.x, &sensors.Z.P, sensors_fuseZ_transition, sensors_fuseZ_measurement, NULL);
    // Kalman Filter Y initialisieren
    EEKF_CALLOC_MATRIX(sensors.Y.x, 2, 1); // 2 States: Position, Geschwindigkeit
    EEKF_CALLOC_MATRIX(sensors.Y.P, 2, 2);
    EEKF_CALLOC_MATRIX(sensors.Y.z, 2, 1); // 1 Messung: GPS, GPS-Geschwindigkeit
    sensors_fuseY_reset();
    eekf_init(&sensors.Y.ekf, &sensors.Y.x, &sensors.Y.P, sensors_fuseY_transition, sensors_fuseY_measurement, NULL);
    // Kalman Filter X initialisieren
    EEKF_CALLOC_MATRIX(sensors.X.x, 2, 1); // 2 States: Position, Geschwindigkeit
    EEKF_CALLOC_MATRIX(sensors.X.P, 2, 2);
    EEKF_CALLOC_MATRIX(sensors.X.z, 1, 1); // 1 Messung: GPS
    sensors_fuseX_reset();
    eekf_init(&sensors.X.ekf, &sensors.X.x, &sensors.X.P, sensors_fuseX_transition, sensors_fuseX_measurement, NULL);
    // installiere task
    if (xTaskCreate(&sensors_task, "sensors", 3 * 1024, NULL, xSensors_PRIORITY, &xSensors_handle) != pdTRUE) return true;
    return ret;
}

void sensors_setHome() {
    bno_setHome(); // nur Barometer
    ult_setHome();
    gps_setHome();
    // Fusion zurücksetzen
    sensors_fuseZ_reset();
    sensors_fuseY_reset();
    sensors_fuseX_reset();
    return;
}


/* Haupttask */

void sensors_task(void* arg) {
    // Variablen
    struct sensors_input_t input;
    // Loop
    while (true) {
        if (xQueueReceive(xSensors_input, &input, 5000 / portTICK_PERIOD_MS) == pdTRUE) {
            switch (input.type) {
                case (SENSORS_ACCELERATION): {
                    ESP_LOGI("sensors", "%llu,A,%f,%f,%f,%f", input.timestamp, input.vector.x, input.vector.y, input.vector.z, input.accuracy);
                    // sensors_fuseX(input.type, input.vector.x, input.timestamp);
                    // sensors_fuseY(input.type, input.vector.y, input.timestamp);
                    // sensors_fuseZ(input.type, input.vector.z, input.timestamp);
                    break;
                }
                case (SENSORS_ORIENTATION): {
                    // ESP_LOGI("sensors", "%llu,O,%f,%f,%f,%f,%f", input.timestamp, input.orientation.i, input.orientation.j, input.orientation.k, input.orientation.real, input.accuracy);
                    break;
                }
                case (SENSORS_ALTIMETER): {
                    // ESP_LOGI("sensors", "%llu,B,%f,%f", input.timestamp, input.distance, input.accuracy);
                    // sensors_fuseZ(input.type, input.distance, input.timestamp);
                    break;
                }
                case (SENSORS_ULTRASONIC): {
                    // ESP_LOGI("sensors", "%llu,U,%f", input.timestamp, input.distance);
                    // sensors_fuseZ(input.type, input.distance, input.timestamp);
                    break;
                }
                case (SENSORS_POSITION): {
                    ESP_LOGI("sensors", "%llu,P,%f,%f,%f,%f", input.timestamp, input.vector.x, input.vector.y, input.vector.z, input.accuracy);
                    // sensors_fuseX(input.type, input.vector.x, input.timestamp);
                    // sensors_fuseY(input.type, input.vector.y, input.timestamp);
                    // sensors_fuseZ(input.type, input.vector.z, input.timestamp);
                    break;
                }
                case (SENSORS_GROUNDSPEED): {
                    ESP_LOGI("sensors", "%llu,S,%f,%f,%f", input.timestamp, input.vector.x, input.vector.y, input.accuracy);
                    // sensors_fuseX(input.type, input.vector.x, input.timestamp);
                    // sensors_fuseY(input.type, input.vector.y, input.timestamp);
                    // sensors_fuseZ(input.type, input.vector.z, input.timestamp);
                    break;
                }
                default:
                    continue;
            }
            // lösche wenn Platz gering wird
            if (uxQueueSpacesAvailable(xSensors_input) <= 1) {
                xQueueReset(xSensors_input);
                ESP_LOGE("sensors", "queue reset!");
            }
            // Timeouterkennung sh2_reinitialize(void); ?
            sensors.timeouts[input.type] = input.timestamp;
            input.timestamp = input.timestamp - (SENSORS_TIMEOUT_MS * 1000);
            for (uint8_t i = 0; i < SENSORS_POSITION; ++i) { // ignoriere vorerst GPS
                if (sensors.timeouts[i] < input.timestamp) {
                    ESP_LOGE("sensors", "timeout of sensor %u", i);
                }
            }
        } else {
            ESP_LOGD("sensors", "%llu,online", esp_timer_get_time());
        }
    }
}


/* Z Altitute Fusion */

static void sensors_fuseZ_reset() {
    // Zustand
    *EEKF_MAT_EL(sensors.Z.x, 0, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.Z.x, 1, 0) = 0.0f;
    // Unsicherheit
    *EEKF_MAT_EL(sensors.Z.P, 0, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.Z.P, 0, 1) = 0.0f;
    *EEKF_MAT_EL(sensors.Z.P, 1, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.Z.P, 1, 1) = 1.0f;
    // Messung
    *EEKF_MAT_EL(sensors.Z.z, 0, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.Z.z, 1, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.Z.z, 2, 0) = 0.0f;
    // DEBUG
    ESP_LOGV("sensors", "fuseZ reset");
}

static void sensors_fuseZ(enum sensors_input_type_t type, float z, int64_t timestamp) {
    // Voraussagen oder Korrigieren
    if (type == SENSORS_ACCELERATION) { // Voraussagen
        // vergangene Zeit
        if (timestamp < sensors.Z.lastTimestamp) return; // verspätete Messung
        float dt = (timestamp - sensors.Z.lastTimestamp) / 1000.0f / 1000.0f;
        sensors.Z.lastTimestamp = timestamp;
        sensors.Z.ekf.userData = (void*) &dt;
        // Input
        EEKF_DECL_MAT_INIT(u, 1, 1, z);
        // Unsicherheit der Voraussage
        z = fabsf(z) + SENSORS_FUSE_Z_ERROR_ACCELERATION;
        EEKF_DECL_MAT(Q, 2, 2);
        *EEKF_MAT_EL(Q, 0, 0) = 0.25f * z * powf(dt, 4.0f);
        *EEKF_MAT_EL(Q, 0, 1) = 0.5f * z * powf(dt, 3.0f);
        *EEKF_MAT_EL(Q, 1, 0) = 0.5f * z * powf(dt, 3.0f);
        *EEKF_MAT_EL(Q, 1, 1) = z * dt * dt;
        // Ausführen
        eekf_return ret;
        ret = eekf_predict(&sensors.Z.ekf, &u, &Q);
        if (ret != eEekfReturnOk) { // Rechenfehler
            ESP_LOGE("sensors", "fuseZ predict error: %u", ret);
        }
    } else { // Korrigieren
        sensors.Z.ekf.userData = (void*) &type;
        // Mssunsicherheit
        EEKF_DECL_MAT_INIT(R, 3, 3, 0);
        switch (type) {
            case (SENSORS_ULTRASONIC):
                *EEKF_MAT_EL(sensors.Z.z, 0, 0) = z;
                *EEKF_MAT_EL(R, 0, 0) = SENSORS_FUSE_Z_ERROR_ULTRASONIC;
                break;
            case (SENSORS_ALTIMETER):
                *EEKF_MAT_EL(sensors.Z.z, 1, 0) = z;
                *EEKF_MAT_EL(R, 1, 1) = SENSORS_FUSE_Z_ERROR_BAROMETER;
                break;
            case (SENSORS_POSITION):
                *EEKF_MAT_EL(sensors.Z.z, 2, 0) = z;
                *EEKF_MAT_EL(R, 2, 2) = SENSORS_FUSE_Z_ERROR_GPS;
            default:
                return; // unbekannter Sensortyp
        }
        // Ausführen
        eekf_return ret;
        ret = eekf_lazy_correct(&sensors.Z.ekf, &sensors.Z.z, &R);
        if (ret != eEekfReturnOk) { // Rechenfehler
            ESP_LOGE("sensors", "fuseZ correct error: %u", ret);
        }
    }
    // DEBUG
    ESP_LOGD("sensors", "Fz,%f,%f,Z,%f,%f,%f", *EEKF_MAT_EL(sensors.Z.x, 0, 0), *EEKF_MAT_EL(sensors.Z.x, 1, 0), *EEKF_MAT_EL(sensors.Z.z, 0, 0), *EEKF_MAT_EL(sensors.Z.z, 1, 0), *EEKF_MAT_EL(sensors.Z.z, 2, 0));
}

eekf_return sensors_fuseZ_transition(eekf_mat* xp, eekf_mat* Jf, eekf_mat const *x,
                                     eekf_mat const *u, void* userData) {
    float dt = *((float*) userData);
    // Physikmodell an dt anpassen
    *EEKF_MAT_EL(*Jf, 0, 0) = 1.0f;
    *EEKF_MAT_EL(*Jf, 0, 1) = dt;
    *EEKF_MAT_EL(*Jf, 1, 0) = 0.0f;
    *EEKF_MAT_EL(*Jf, 1, 1) = 1.0f;
    // gemäss Modell vorausrechnen
    // xp = F * x
    if (NULL == eekf_mat_mul(xp, Jf, x)) return eEekfReturnComputationFailed;
    // Beschleunigung als Input dazurechnen
    EEKF_DECL_MAT_INIT(G, 2, 1, 0.5f * dt * dt, dt);
    EEKF_DECL_MAT_INIT(gu, 2, 1, 0);
    // xp += G * u
    if (NULL == eekf_mat_add(xp, xp, eekf_mat_mul(&gu, &G, u))) return eEekfReturnComputationFailed;
    // Limits einhalten
    float *predictedVelocity = EEKF_MAT_EL(*xp, 1, 0);
    if (*predictedVelocity > SENSORS_FUSE_Z_LIMIT_VEL) *predictedVelocity = SENSORS_FUSE_Z_LIMIT_VEL;
    else if (*predictedVelocity < -SENSORS_FUSE_Z_LIMIT_VEL) *predictedVelocity = -SENSORS_FUSE_Z_LIMIT_VEL;
    return eEekfReturnOk;
}

eekf_return sensors_fuseZ_measurement(eekf_mat* zp, eekf_mat* Jh, eekf_mat const *x, void* userData) {
    enum sensors_input_type_t type = *((enum sensors_input_type_t*) userData);
    // Messmodell an Messung anpassen
    memset(Jh->elements, 0.0f, sizeof(eekf_value) * Jh->rows * Jh->cols);
    switch (type) {
        case (SENSORS_ULTRASONIC):
            *EEKF_MAT_EL(*Jh, 0, 0) = 1.0f;
            break;
        case (SENSORS_ALTIMETER):
            *EEKF_MAT_EL(*Jh, 1, 0) = 1.0f;
            break;
        case (SENSORS_POSITION):
            *EEKF_MAT_EL(*Jh, 2, 0) = 1.0f;
            break;
        default:
            return eEekfReturnParameterError;
    }
    // zp = H * x
    if (NULL == eekf_mat_mul(zp, Jh, x)) return eEekfReturnComputationFailed;
	return eEekfReturnOk;
}


/* Y Longitude Fusion */

static void sensors_fuseY_reset() {
    // Zustand
    *EEKF_MAT_EL(sensors.Y.x, 0, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.Y.x, 1, 0) = 0.0f;
    // Unsicherheit
    *EEKF_MAT_EL(sensors.Y.P, 0, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.Y.P, 0, 1) = 0.0f;
    *EEKF_MAT_EL(sensors.Y.P, 1, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.Y.P, 1, 1) = 1.0f;
    // Messung
    *EEKF_MAT_EL(sensors.Y.z, 0, 0) = 0.0f;
    // DEBUG
    ESP_LOGV("sensors", "fuseY reset");
}

static void sensors_fuseY(enum sensors_input_type_t type, float y, int64_t timestamp) {
    // Voraussagen oder Korrigieren
    if (type == SENSORS_ACCELERATION) { // Voraussagen
        // vergangene Zeit
        if (timestamp < sensors.Y.lastTimestamp) return; // verspätete Messung
        float dt = (timestamp - sensors.Y.lastTimestamp) / 1000.0f / 1000.0f;
        sensors.Y.lastTimestamp = timestamp;
        sensors.Y.ekf.userData = (void*) &dt;
        // Input
        EEKF_DECL_MAT_INIT(u, 1, 1, y);
        // Unsicherheit der Voraussage
        y = fabsf(y) + SENSORS_FUSE_Y_ERROR_ACCELERATION;
        EEKF_DECL_MAT(Q, 2, 2);
        *EEKF_MAT_EL(Q, 0, 0) = 0.25f * y * powf(dt, 4.0f);
        *EEKF_MAT_EL(Q, 0, 1) = 0.5f * y * powf(dt, 3.0f);
        *EEKF_MAT_EL(Q, 1, 0) = 0.5f * y * powf(dt, 3.0f);
        *EEKF_MAT_EL(Q, 1, 1) = y * dt * dt;
        // Ausführen
        eekf_return ret;
        ret = eekf_predict(&sensors.Y.ekf, &u, &Q);
        if (ret != eEekfReturnOk) { // Rechenfehler
            ESP_LOGE("sensors", "fuseY predict error: %u", ret);
        }
    } else { // Korrigieren
        sensors.Y.ekf.userData = (void*) &type;
        // Messunsicherheit
        EEKF_DECL_MAT_INIT(R, 2, 2, 0);
        switch (type) {
            case (SENSORS_POSITION):
                *EEKF_MAT_EL(sensors.Y.z, 0, 0) = y;
                *EEKF_MAT_EL(R, 0, 0) = SENSORS_FUSE_Y_ERROR_GPS;
            case (SENSORS_GROUNDSPEED):
                *EEKF_MAT_EL(sensors.Y.z, 1, 0) = y;
                *EEKF_MAT_EL(R, 1, 1) = SENSORS_FUSE_Y_ERROR_VELOCITY;
            default:
                return; // unbekannter Sensortyp
        }
        // Ausführen
        eekf_return ret;
        ret = eekf_lazy_correct(&sensors.Y.ekf, &sensors.Y.z, &R);
        if (ret != eEekfReturnOk) { // Rechenfehler
            ESP_LOGE("sensors", "fuseY correct error: %u", ret);
        }
    }
    // DEBUG
    ESP_LOGD("sensors", "Fy,%f,%f,Z,%f,%f", *EEKF_MAT_EL(sensors.Y.x, 0, 0), *EEKF_MAT_EL(sensors.Y.x, 1, 0), *EEKF_MAT_EL(sensors.Y.z, 0, 0), *EEKF_MAT_EL(sensors.Y.z, 1, 0));
}

eekf_return sensors_fuseY_transition(eekf_mat* xp, eekf_mat* Jf, eekf_mat const *x,
                                     eekf_mat const *u, void* userData) {
    float dt = *((float*) userData);
    // Physikmodell an dt anpassen
    *EEKF_MAT_EL(*Jf, 0, 0) = 1.0f;
    *EEKF_MAT_EL(*Jf, 0, 1) = dt;
    *EEKF_MAT_EL(*Jf, 1, 0) = 0.0f;
    *EEKF_MAT_EL(*Jf, 1, 1) = 1.0f;
    // gemäss Modell vorausrechnen
    // xp = F * x
    if (NULL == eekf_mat_mul(xp, Jf, x)) return eEekfReturnComputationFailed;
    // Beschleunigung als Input dazurechnen
    EEKF_DECL_MAT_INIT(G, 2, 1, 0.5f * dt * dt, dt);
    EEKF_DECL_MAT_INIT(gu, 2, 1, 0);
    // xp += G * u
    if (NULL == eekf_mat_add(xp, xp, eekf_mat_mul(&gu, &G, u))) return eEekfReturnComputationFailed;
    // Limits einhalten
    float *predictedVelocity = EEKF_MAT_EL(*xp, 1, 0);
    if (*predictedVelocity > SENSORS_FUSE_Y_LIMIT_VEL) *predictedVelocity = SENSORS_FUSE_Y_LIMIT_VEL;
    else if (*predictedVelocity < -SENSORS_FUSE_Y_LIMIT_VEL) *predictedVelocity = -SENSORS_FUSE_Y_LIMIT_VEL;
    return eEekfReturnOk;
}

eekf_return sensors_fuseY_measurement(eekf_mat* zp, eekf_mat* Jh, eekf_mat const *x, void* userData) {
    enum sensors_input_type_t type = *((enum sensors_input_type_t*) userData);
    // Messmodell an Messung anpassen
    memset(Jh->elements, 0.0f, sizeof(eekf_value) * Jh->rows * Jh->cols);
    switch (type) {
        case (SENSORS_POSITION):
            *EEKF_MAT_EL(*Jh, 0, 0) = 1.0f;
            break;
        case (SENSORS_GROUNDSPEED):
            *EEKF_MAT_EL(*Jh, 1, 1) = 1.0f;
        default:
            return eEekfReturnParameterError;
    }
    // zp = H * x
    if (NULL == eekf_mat_mul(zp, Jh, x)) return eEekfReturnComputationFailed;
	return eEekfReturnOk;
}


/* X Longitude Fusion */

static void sensors_fuseX_reset() {
    // Zustand
    *EEKF_MAT_EL(sensors.X.x, 0, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.X.x, 1, 0) = 0.0f;
    // Unsicherheit
    *EEKF_MAT_EL(sensors.X.P, 0, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.X.P, 0, 1) = 0.0f;
    *EEKF_MAT_EL(sensors.X.P, 1, 0) = 0.0f;
    *EEKF_MAT_EL(sensors.X.P, 1, 1) = 1.0f;
    // Messung
    *EEKF_MAT_EL(sensors.X.z, 0, 0) = 0.0f;
    // DEBUG
    ESP_LOGV("sensors", "fuseX reset");
}

static void sensors_fuseX(enum sensors_input_type_t type, float x, int64_t timestamp) {
    // Voraussagen oder Korrigieren
    if (type == SENSORS_ACCELERATION) { // Voraussagen
        // vergangene Zeit
        if (timestamp < sensors.X.lastTimestamp) return; // verspätete Messung
        float dt = (timestamp - sensors.X.lastTimestamp) / 1000.0f / 1000.0f;
        sensors.X.lastTimestamp = timestamp;
        sensors.X.ekf.userData = (void*) &dt;
        // Input
        EEKF_DECL_MAT_INIT(u, 1, 1, x);
        // Unsicherheit der Voraussage
        x = fabsf(x) + SENSORS_FUSE_X_ERROR_ACCELERATION;
        EEKF_DECL_MAT(Q, 2, 2);
        *EEKF_MAT_EL(Q, 0, 0) = 0.25f * x * powf(dt, 4.0f);
        *EEKF_MAT_EL(Q, 0, 1) = 0.5f * x * powf(dt, 3.0f);
        *EEKF_MAT_EL(Q, 1, 0) = 0.5f * x * powf(dt, 3.0f);
        *EEKF_MAT_EL(Q, 1, 1) = x * dt * dt;
        // Ausführen
        eekf_return ret;
        ret = eekf_predict(&sensors.X.ekf, &u, &Q);
        if (ret != eEekfReturnOk) { // Rechenfehler
            ESP_LOGE("sensors", "fuseX predict error: %u", ret);
        }
    } else { // Korrigieren
        sensors.X.ekf.userData = (void*) &type;
        // Mssunsicherheit
        EEKF_DECL_MAT_INIT(R, 1, 1, 0);
        switch (type) {
            case (SENSORS_POSITION):
                *EEKF_MAT_EL(sensors.X.z, 0, 0) = x;
                *EEKF_MAT_EL(R, 0, 0) = SENSORS_FUSE_X_ERROR_GPS;
            case (SENSORS_GROUNDSPEED): // ToDo?
            default:
                return; // unbekannter Sensortyp
        }
        // Ausführen
        eekf_return ret;
        ret = eekf_lazy_correct(&sensors.X.ekf, &sensors.X.z, &R);
        if (ret != eEekfReturnOk) { // Rechenfehler
            ESP_LOGE("sensors", "fuseX correct error: %u", ret);
        }
    }
    // DEBUG
    ESP_LOGD("sensors", "Fx,%f,%f,Z,%f", *EEKF_MAT_EL(sensors.X.x, 0, 0), *EEKF_MAT_EL(sensors.X.x, 1, 0), *EEKF_MAT_EL(sensors.X.z, 0, 0));
}

eekf_return sensors_fuseX_transition(eekf_mat* xp, eekf_mat* Jf, eekf_mat const *x,
                                     eekf_mat const *u, void* userData) {
    float dt = *((float*) userData);
    // Physikmodell an dt anpassen
    *EEKF_MAT_EL(*Jf, 0, 0) = 1.0f;
    *EEKF_MAT_EL(*Jf, 0, 1) = dt;
    *EEKF_MAT_EL(*Jf, 1, 0) = 0.0f;
    *EEKF_MAT_EL(*Jf, 1, 1) = 1.0f;
    // gemäss Modell vorausrechnen
    // xp = F * x
    if (NULL == eekf_mat_mul(xp, Jf, x)) return eEekfReturnComputationFailed;
    // Beschleunigung als Input dazurechnen
    EEKF_DECL_MAT_INIT(G, 2, 1, 0.5f * dt * dt, dt);
    EEKF_DECL_MAT_INIT(gu, 2, 1, 0);
    // xp += G * u
    if (NULL == eekf_mat_add(xp, xp, eekf_mat_mul(&gu, &G, u))) return eEekfReturnComputationFailed;
    // Limits einhalten
    float *predictedVelocity = EEKF_MAT_EL(*xp, 1, 0);
    if (*predictedVelocity > SENSORS_FUSE_X_LIMIT_VEL) *predictedVelocity = SENSORS_FUSE_X_LIMIT_VEL;
    else if (*predictedVelocity < -SENSORS_FUSE_X_LIMIT_VEL) *predictedVelocity = -SENSORS_FUSE_X_LIMIT_VEL;
    return eEekfReturnOk;
}

eekf_return sensors_fuseX_measurement(eekf_mat* zp, eekf_mat* Jh, eekf_mat const *x, void* userData) {
    enum sensors_input_type_t type = *((enum sensors_input_type_t*) userData);
    // Messmodell an Messung anpassen
    memset(Jh->elements, 0.0f, sizeof(eekf_value) * Jh->rows * Jh->cols);
    switch (type) {
        case (SENSORS_POSITION):
            *EEKF_MAT_EL(*Jh, 0, 0) = 1.0f;
            break;
        // case (SENSORS_GROUNDSPEED):
        //     *EEKF_MAT_EL(*Jh, 0, 1) = 1.0f;
        default:
            return eEekfReturnParameterError;
    }
    // zp = H * x
    if (NULL == eekf_mat_mul(zp, Jh, x)) return eEekfReturnComputationFailed;
	return eEekfReturnOk;
}
