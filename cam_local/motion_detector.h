#ifndef MOTION_DETECTOR_H
#define MOTION_DETECTOR_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
    #endif

    // Parametry detekcji ruchu
    typedef struct {
        int motionThreshold;  // próg różnicy (0-255), domyślnie 20
        int minArea;          // minimalna powierzchnia konturu, domyślnie 200
        int gaussBlur;        // rozmiar kernela Gaussa (nieparzysta), domyślnie 21
    } MotionParams;

    /**
     * Inicjalizacja detektora ruchu
     * Zwraca: wskaźnik do wewnętrznego stanu (nieprzezroczysty)
     */
    void* motion_detector_init(int width, int height, MotionParams params);

    /**
     * Detekcja ruchu między dwoma bufferami MJPEG
     * 
     * @param detector - wskaźnik zwrócony przez motion_detector_init
     * @param currentBuffer - bufor z aktualną klatką MJPEG
     * @param currentSize - rozmiar bufora currentBuffer
     * @param prevBuffer - bufor z poprzednią klatką MJPEG (może być NULL przy pierwszym wywołaniu)
     * @param prevSize - rozmiar bufora prevBuffer
     * 
     * @return true jeśli wykryto ruch, false w przeciwnym razie
     */
    bool motion_detector_detect(void* detector, 
                                const unsigned char* currentBuffer, size_t currentSize,
                                const unsigned char* prevBuffer, size_t prevSize);

    /**
     * Zwolnienie zasobów detektora
     */
    void motion_detector_destroy(void* detector);

    #ifdef __cplusplus
}
#endif

#endif // MOTION_DETECTOR_H