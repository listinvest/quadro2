#!/usr/bin/python3
# -*- coding: utf-8 -*
import sys
import csv
import numpy as np
import matplotlib.pyplot as plt
import math
import warnings
warnings.simplefilter('error', RuntimeWarning)

# Kalman Filter
# Physik basierend auf konstanter Geschwindigkeit
# bei Beschleunigungsmessung wird Status vorausgesagt, resp. Geschwindigkeit integriert per Input
# bei den anderen Messungen wird nur korrigiert.

def main():
    dT = 0
    x = np.matrix([[0.0], [0.0]]) # Höhe, Geschwindigkeit
    z = np.matrix([[0.0], [0.0], [0.0]]) # Ultraschall, Barometer, GPS
    P = np.diag([0.0, 0.1]) # anfängliche Unsicherheit 0.1
    H = np.matrix([[0.0, 0.0],
                   [0.0, 0.0],
                   [0.0, 0.0]])
    rU = 0.005 / 2
    rB = 0.3 / 2
    rP = 10000000.0 / 2
    #R = np.diag([rU**2, rB**2, rP**2]) # Unsicherheit der Messung (per Messung)
    F = np.identity(2) # Physikmodell (muss um dT in (0,1) angepasst werden)
    G = np.matrix([[0.0], [0.0]]) # Inputmodell (muss um dT angepasst werden)
    I = np.identity(2)
    t = [0.0]
    f = [[0.0], [0.0], [0.0]]
    u = [[0.0], [0.0], [0.0]]
    b = [[0.0], [0.0], [0.0]]
    v = [[0.0], [0.0], [0.0]]
    p = [0.0]
    d = [0.0]
    num = 0
    numSkip = 0
    offsetUltrasonic = 0.0
    offsetBarometer = 0.0

    with open(str(sys.argv[1])) as csvfile:
        readCSV = csv.reader(csvfile, delimiter=';')
        rows = iter(readCSV)
        row = next(rows)
        # Offsets aus erster Zeile extrahieren
        lastTimestamp = int(row[1])
        offsetUltrasonic = float(row[3])
        offsetBarometer = float(row[5])
        # über Zeilen iterieren
        while True:
            try:
                # Daten aus Zeile extrahieren
                row = next(rows)
                num = num + 1
                mType = row[2] # art der Messung: A, U oder B
                print(num, "Messung", mType)
                timestamp = int(row[1])
                dT = (timestamp - lastTimestamp) / 1000 / 1000 # vergangene Zeit (in s)
                if dT < 0:
                    numSkip = numSkip + 1
                    print("skip", mType)
                    continue
                lastTimestamp = timestamp
                if mType == 'A': # Beschleunigung
                    mValue = float(row[5])
                elif mType == 'U': # Ultraschall
                    mValue = float(row[3]) - offsetUltrasonic
                elif mType == 'B': # Barometer
                    mValue = float(row[3]) - offsetBarometer
                else:
                    print("Messung übersprungen:", mType)
                    continue


                # Predict
                if mType == 'A': # Beschleunigung -> Voraussage
                    # Matrizen anpassen
                    F.itemset((0, 1), dT) # Physik
                    G.itemset((0, 0), 0.5 * dT**2) # Input
                    G.itemset((1, 0), dT) # Input
                    # Beschleunigung zur Geschwindigkeit integrieren
                    xHat = F * x + G * mValue
                    Q = G * G.T * (abs(mValue) + 0.35)
                    PHat = (F * P * F.T) + Q # + np.diag([0.0, 0.01])
                    x = xHat
                    P = PHat
                    #continue # nur Vorraussage bei Beschleunigung
                else:
                    if mType == 'U': # Ultraschall -> Korrektur
                        if 2000 < num < 2500:
                            continue
                        R = np.diag([rU**2, 0.0, 0.0])
                        z.itemset(0, mValue)
                        H = np.matrix([[1.0, 0.0],
                                    [0.0, 0.0],
                                    [0.0, 0.0]])
                    elif mType == 'B': # Barometer -> Korrektur
                        R = np.diag([0.0, rB**2, 0.0])
                        z.itemset(1, mValue)
                        H = np.matrix([[0.0, 0.0],
                                    [1.0, 0.0],
                                    [0.0, 0.0]])
                    elif mType == 'P': # GPS -> Korrektur
                        R = np.diag([0.0, 0.0, rP**2])
                        z.itemset(2, mValue)
                        H = np.matrix([[0.0, 0.0],
                                    [0.0, 0.0],
                                    [1.0, 0.0]])

                    # Gain rechnen und Korrigieren
                    S = (H * P * H.T) + R
                    print(S)
                    if S.item((0, 0)) != 0:
                        S.itemset((0, 0), 1.0 / S.item((0, 0)))
                    if S.item((1, 1)) != 0:
                        S.itemset((1, 1), 1.0 / S.item((1, 1)))
                    if S.item((2, 2)) != 0:
                        S.itemset((2, 2), 1.0 / S.item((2, 2)))
                    K = (P * H.T) * S #np.linalg.pinv(S)
                    P = (I - (K * H)) * P
                    x = x + K * (z - (H * x))

                # speichere für Plot
                t.append(t[-1] + dT)
                f[0].append(x.item(0))
                f[1].append(f[0][-1] - 2 * np.sqrt(abs(P[0, 0])))
                f[2].append(f[0][-1] + 2 * np.sqrt(abs(P[0, 0])))
                v[0].append(x.item(1))
                v[1].append(v[0][-1] - 2 * np.sqrt(abs(P[1, 1])))
                v[2].append(v[0][-1] + 2 * np.sqrt(abs(P[1, 1])))
                u[0].append(z.item(0))
                b[0].append(z.item(1))

            except StopIteration:
                break
            except np.linalg.LinAlgError:
                print("Singuläre Matrix in Schritt", num)
                break


        print(numSkip, "von", num, "Messungen übersprungen", 100.0 / num * numSkip)
        # Resultat
        plt.title('Fusion')
        if len(sys.argv) > 2:
            toPlot = sys.argv[2]
        else:
            toPlot = "fvub2"
        if "f" in toPlot:
            plt.plot(t, f[0], 'b', label='Fusion')
            if "fe" in toPlot:
                plt.plot(t, f[1], 'b--')
                plt.plot(t, f[2], 'b--')
        if "v" in toPlot:
            plt.plot(t, v[0], 'r', label='Geschwindigkeit')
            if "ve" in toPlot:
                plt.plot(t, v[1], 'r--')
                plt.plot(t, v[2], 'r--')
        if "u" in toPlot:
            plt.plot(t, u[0], 'g', label='Ultraschall')
        if "b" in toPlot:
            plt.plot(t, b[0], 'y', label='Barometer')
        if "2" in toPlot:
            plt.ylim(-2.0, +2.0)
        plt.legend()
        plt.show()

        #plt.matshow(P, cmap='binary')
        #plt.title('Warscheinlichkeitsverteilung')
        #plt.show()
        #plt.matshow(K, cmap='binary')
        #plt.title('Kalman Gain')
        #plt.show()

main()
exit()
