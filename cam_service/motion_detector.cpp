#include "motion_detector.h"
#include <opencv2/opencv.hpp>
#include <vector>

static int g_motionCallCounter = 0;
using namespace cv;

// Wewnętrzna struktura stanu detektora
struct MotionDetectorState {
    MotionParams params;
    int width;
    int height;
};

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

    // jeśli to pierwsze wywołanie lub brak poprzedniej klatki
    if (!prevBuffer || prevSize == 0)
    {
        return false;
    }

    // dekoduj aktualne klatki z MJPEG
    std::vector<uchar> currentVec(currentBuffer, currentBuffer + currentSize);
    Mat currentFrame = imdecode(currentVec, IMREAD_COLOR);
    
    std::vector<uchar> prevVec(prevBuffer, prevBuffer + prevSize);
    Mat prevFrame = imdecode(prevVec, IMREAD_COLOR);

    if (currentFrame.empty() || prevFrame.empty())
    {
        return false;
    }

    // konwersja do skali szarości
    Mat gray, prevGray;
    cvtColor(currentFrame, gray, COLOR_BGR2GRAY);
    cvtColor(prevFrame, prevGray, COLOR_BGR2GRAY);

    // rozmycie gaussa
    GaussianBlur(gray, gray, Size(state->params.gaussBlur, state->params.gaussBlur), 0);
    GaussianBlur(prevGray, prevGray, Size(state->params.gaussBlur, state->params.gaussBlur), 0);

    // różnica klatek
    Mat frameDelta;
    absdiff(prevGray, gray, frameDelta);

    // progowanie
    Mat thresh;
    threshold(frameDelta, thresh, state->params.motionThreshold, 255, THRESH_BINARY);

    // pogrubienie
    Mat kernel = getStructuringElement(MORPH_RECT, Size(3, 3));
    dilate(thresh, thresh, kernel, Point(-1, -1), 2);

    // znajdowanie konturów
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

    return motionFound;
}

void motion_detector_destroy(void* detector)
{
    if (detector)
    {
        MotionDetectorState* state = static_cast<MotionDetectorState*>(detector);
        delete state;
    }
}