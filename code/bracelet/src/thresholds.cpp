#include <Arduino.h>
#include <Wire.h>
#include "SparkFun_BMI270_Arduino_Library.h"

namespace
{
    namespace Pin
    {
        constexpr uint8_t IMU_SDA = 3;
        constexpr uint8_t IMU_SCL = 2;
    }

    constexpr uint32_t SERIAL_BAUD = 115200;
    constexpr uint32_t SAMPLE_PERIOD_MS = 20;
    constexpr uint16_t STILL_SAMPLE_COUNT = 1000;
    constexpr uint16_t ACTIVE_SAMPLE_COUNT = 1500;
    constexpr uint16_t MAX_SAMPLE_COUNT = ACTIVE_SAMPLE_COUNT;
    constexpr uint32_t WARMUP_MS = 2000;

    BMI270 imu;

    struct MotionSample
    {
        float accelDeltaG = 0.0F;
        float gyroMagnitudeDps = 0.0F;
    };

    struct MotionStats
    {
        float meanAccelDeltaG = 0.0F;
        float meanGyroMagnitudeDps = 0.0F;
        float p95AccelDeltaG = 0.0F;
        float p95GyroMagnitudeDps = 0.0F;
        float p99AccelDeltaG = 0.0F;
        float p99GyroMagnitudeDps = 0.0F;
        float maxAccelDeltaG = 0.0F;
        float maxGyroMagnitudeDps = 0.0F;
    };

    MotionSample stillSamples[STILL_SAMPLE_COUNT];
    MotionSample activeSamples[ACTIVE_SAMPLE_COUNT];
    float sortBuffer[MAX_SAMPLE_COUNT];

    float magnitude3(float x, float y, float z)
    {
        return sqrtf((x * x) + (y * y) + (z * z));
    }

    bool beginBmi270()
    {
        const uint8_t addresses[] = {BMI2_I2C_PRIM_ADDR, BMI2_I2C_SEC_ADDR};

        for (uint8_t address : addresses)
        {
            if (imu.beginI2C(address, Wire) == BMI2_OK)
            {
                Serial.print("BMI270 trouve a l'adresse 0x");
                Serial.println(address, HEX);
                return true;
            }
        }

        Serial.println("ERREUR: BMI270 introuvable sur 0x68 ou 0x69.");
        return false;
    }

    void waitForEnter()
    {
        while (Serial.available() > 0)
        {
            Serial.read();
        }

        while (true)
        {
            if (Serial.available() > 0)
            {
                while (Serial.available() > 0)
                {
                    Serial.read();
                }
                return;
            }
            delay(20);
        }
    }

    bool readMotionSample(MotionSample &sample)
    {
        const int8_t status = imu.getSensorData();
        if (status != BMI2_OK)
        {
            Serial.print("ERREUR lecture BMI270: ");
            Serial.println(status);
            return false;
        }

        sample.accelDeltaG = fabsf(magnitude3(imu.data.accelX, imu.data.accelY, imu.data.accelZ) - 1.0F);
        sample.gyroMagnitudeDps = magnitude3(imu.data.gyroX, imu.data.gyroY, imu.data.gyroZ);
        return true;
    }

    void sortValues(float values[], uint16_t count)
    {
        for (uint16_t i = 1; i < count; i++)
        {
            const float value = values[i];
            int16_t j = static_cast<int16_t>(i) - 1;
            while (j >= 0 && values[j] > value)
            {
                values[j + 1] = values[j];
                j--;
            }
            values[j + 1] = value;
        }
    }

    float percentile(const MotionSample samples[], uint16_t count, bool useAccel, float percent)
    {
        for (uint16_t i = 0; i < count; i++)
        {
            sortBuffer[i] = useAccel ? samples[i].accelDeltaG : samples[i].gyroMagnitudeDps;
        }

        sortValues(sortBuffer, count);
        const uint16_t index = static_cast<uint16_t>(roundf((percent / 100.0F) * static_cast<float>(count - 1)));
        return sortBuffer[index];
    }

    MotionStats calculateStats(const MotionSample samples[], uint16_t count)
    {
        MotionStats stats;

        for (uint16_t i = 0; i < count; i++)
        {
            stats.meanAccelDeltaG += samples[i].accelDeltaG;
            stats.meanGyroMagnitudeDps += samples[i].gyroMagnitudeDps;
            stats.maxAccelDeltaG = max(stats.maxAccelDeltaG, samples[i].accelDeltaG);
            stats.maxGyroMagnitudeDps = max(stats.maxGyroMagnitudeDps, samples[i].gyroMagnitudeDps);
        }

        stats.meanAccelDeltaG /= count;
        stats.meanGyroMagnitudeDps /= count;
        stats.p95AccelDeltaG = percentile(samples, count, true, 95.0F);
        stats.p95GyroMagnitudeDps = percentile(samples, count, false, 95.0F);
        stats.p99AccelDeltaG = percentile(samples, count, true, 99.0F);
        stats.p99GyroMagnitudeDps = percentile(samples, count, false, 99.0F);
        return stats;
    }

    bool capturePhase(const char *label, MotionSample samples[], uint16_t count)
    {
        Serial.println();
        Serial.print("Phase: ");
        Serial.println(label);
        Serial.print("Duree: ");
        Serial.print((count * SAMPLE_PERIOD_MS) / 1000);
        Serial.println(" secondes.");
        Serial.println("Appuie sur Entree pour demarrer.");
        waitForEnter();

        Serial.println("Mesure en cours...");
        uint32_t nextSampleMs = millis();

        for (uint16_t sample = 0; sample < count; sample++)
        {
            while (millis() < nextSampleMs)
            {
                delay(1);
            }
            nextSampleMs += SAMPLE_PERIOD_MS;

            if (!readMotionSample(samples[sample]))
            {
                return false;
            }

            if ((sample + 1) % 100 == 0)
            {
                Serial.print(".");
            }
        }

        Serial.println();
        return true;
    }

    void printStats(const char *label, const MotionStats &stats)
    {
        Serial.println();
        Serial.print("=== ");
        Serial.print(label);
        Serial.println(" ===");
        Serial.print("accelDelta moyen: ");
        Serial.print(stats.meanAccelDeltaG, 6);
        Serial.println(" g");
        Serial.print("accelDelta p95: ");
        Serial.print(stats.p95AccelDeltaG, 6);
        Serial.println(" g");
        Serial.print("accelDelta p99: ");
        Serial.print(stats.p99AccelDeltaG, 6);
        Serial.println(" g");
        Serial.print("accelDelta max: ");
        Serial.print(stats.maxAccelDeltaG, 6);
        Serial.println(" g");
        Serial.print("gyroMagnitude moyen: ");
        Serial.print(stats.meanGyroMagnitudeDps, 6);
        Serial.println(" deg/s");
        Serial.print("gyroMagnitude p95: ");
        Serial.print(stats.p95GyroMagnitudeDps, 6);
        Serial.println(" deg/s");
        Serial.print("gyroMagnitude p99: ");
        Serial.print(stats.p99GyroMagnitudeDps, 6);
        Serial.println(" deg/s");
        Serial.print("gyroMagnitude max: ");
        Serial.print(stats.maxGyroMagnitudeDps, 6);
        Serial.println(" deg/s");
    }

    float midpoint(float low, float high)
    {
        if (high <= low)
        {
            return low;
        }
        return low + ((high - low) * 0.5F);
    }

    void printSuggestedThresholds(const MotionStats &still, const MotionStats &active)
    {
        const float accelNoiseFloor = still.p99AccelDeltaG * 3.0F;
        const float gyroNoiseFloor = still.p99GyroMagnitudeDps * 3.0F;
        const float accelSuggested = max(accelNoiseFloor, midpoint(still.p99AccelDeltaG, active.p95AccelDeltaG));
        const float gyroSuggested = max(gyroNoiseFloor, midpoint(still.p99GyroMagnitudeDps, active.p95GyroMagnitudeDps));

        Serial.println();
        Serial.println("=== Seuils proposes pour src/main.cpp ===");
        Serial.println("// Compare avec les valeurs actuelles avant de recopier.");
        Serial.print("constexpr float ACTIVE_ACCEL_DELTA_G = ");
        Serial.print(accelSuggested, 3);
        Serial.println("F;");
        Serial.print("constexpr float ACTIVE_GYRO_DPS = ");
        Serial.print(gyroSuggested, 1);
        Serial.println("F;");

        Serial.println();
        Serial.println("Regle de lecture:");
        Serial.println("- Le seuil doit etre nettement au-dessus de la phase immobile.");
        Serial.println("- Il doit rester sous la phase d'activite reelle.");
        Serial.println("- Si l'alarme se valide trop facilement, augmente les seuils.");
        Serial.println("- Si elle ne detecte pas assez le mouvement normal, baisse les seuils.");

        if (active.p95AccelDeltaG <= still.p99AccelDeltaG || active.p95GyroMagnitudeDps <= still.p99GyroMagnitudeDps)
        {
            Serial.println();
            Serial.println("ATTENTION: les phases immobile et active sont proches.");
            Serial.println("Refais la mesure avec une activite plus representative.");
        }
    }

    void printIntro()
    {
        Serial.println();
        Serial.println("=== Mesure des seuils de mouvement BMI270 ===");
        Serial.println("Cet outil ne calibre pas l'accelerometre par orientation.");
        Serial.println("Il mesure ce qui est utile pour le produit:");
        Serial.println("- accelDelta = abs(norme acceleration - 1 g)");
        Serial.println("- gyroMagnitude = norme du gyroscope");
        Serial.println();
        Serial.println("Tu vas faire deux phases:");
        Serial.println("1. immobile: bracelet pose ou porte sans bouger");
        Serial.println("2. activite: mouvement que tu veux accepter pour arreter l'alarme");
    }
}

void setup()
{
    Serial.begin(SERIAL_BAUD);
    delay(1000);

    printIntro();

    Wire.begin(Pin::IMU_SDA, Pin::IMU_SCL);
    Wire.setClock(400000);

    if (!beginBmi270())
    {
        return;
    }

    Serial.print("Stabilisation pendant ");
    Serial.print(WARMUP_MS / 1000);
    Serial.println(" secondes...");
    delay(WARMUP_MS);

    if (!capturePhase("immobile", stillSamples, STILL_SAMPLE_COUNT))
    {
        return;
    }

    if (!capturePhase("activite reelle", activeSamples, ACTIVE_SAMPLE_COUNT))
    {
        return;
    }

    const MotionStats stillStats = calculateStats(stillSamples, STILL_SAMPLE_COUNT);
    const MotionStats activeStats = calculateStats(activeSamples, ACTIVE_SAMPLE_COUNT);

    printStats("Immobile", stillStats);
    printStats("Activite reelle", activeStats);
    printSuggestedThresholds(stillStats, activeStats);

    Serial.println();
    Serial.println("Mesure terminee. Redemarre le bracelet pour refaire une mesure.");
}

void loop()
{
}
