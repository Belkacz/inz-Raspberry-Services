#include "motion_detector.h"
#include <opencv2/opencv.hpp>
#include <vector>

using namespace cv;

// Wewnętrzna struktura stanu detektora
struct MotionDetectorState {
    Mat prevFrame;
    MotionParams params;
    int width;
    int height;
};

// extern "C" {

void* motion_detector_init(int width, int height, MotionParams params)
{
    MotionDetectorState* state = new MotionDetectorState();
    state->width = width;
    state->height = height;
    state->params = params;
    return state;
}

bool motion_detector_detect(void* detector,
                           const unsigned char* currentBuffer, size_t currentSize,
                           const unsigned char* prevBuffer, size_t prevSize)
{
    if (!detector || !currentBuffer || currentSize == 0)
    {
        return false;
    }
    
    MotionDetectorState* state = static_cast<MotionDetectorState*>(detector);
    
    // Dekoduj aktualną klatkę z MJPEG
    std::vector<uchar> currentVec(currentBuffer, currentBuffer + currentSize);
    Mat currentFrame = imdecode(currentVec, IMREAD_COLOR);
    
    if (currentFrame.empty())
    {
        return false;
    }
    
    // Jeśli to pierwsze wywołanie lub brak poprzedniej klatki
    if (!prevBuffer || prevSize == 0 || state->prevFrame.empty())
    {
        state->prevFrame = currentFrame.clone();
        return false;
    }
    
    // Konwersja do skali szarości
    Mat gray, prevGray;
    cvtColor(currentFrame, gray, COLOR_BGR2GRAY);
    cvtColor(state->prevFrame, prevGray, COLOR_BGR2GRAY);
    
    // Rozmycie gaussowskie
    GaussianBlur(gray, gray, Size(state->params.gaussBlur, state->params.gaussBlur), 0);
    GaussianBlur(prevGray, prevGray, Size(state->params.gaussBlur, state->params.gaussBlur), 0);
    
    // Różnica klatek
    Mat frameDelta;
    absdiff(prevGray, gray, frameDelta);
    
    // Progowanie
    Mat thresh;
    threshold(frameDelta, thresh, state->params.motionThreshold, 255, THRESH_BINARY);
    
    // Dylatacja
    Mat kernel = getStructuringElement(MORPH_RECT, Size(3, 3));
    dilate(thresh, thresh, kernel, Point(-1, -1), 2);
    
    // Znajdź kontury
    std::vector<std::vector<Point>> contours;
    findContours(thresh, contours, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);
    
    bool motionFound = false;
    for (size_t i = 0; i < contours.size(); i++)
    {
        double area = contourArea(contours[i]);
        if (area > state->params.minArea)
        {
            motionFound = true;
            break;
        }
    }
    
    // Zapisz obecną klatkę jako poprzednią
    state->prevFrame = currentFrame.clone();
    
    return motionFound;
}

void motion_detector_destroy(void* detector)
{
    if (detector)
    {
        MotionDetectorState* state = static_cast<MotionDetectorState*>(detector);
        state->prevFrame.release();
        delete state;
    }
}

// } // extern "C"