#include "motion_detector.h"
#include <opencv2/opencv.hpp>
#include <vector>

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
    if(!detector || !currentBuffer || currentSize == 0)
        return false;

    MotionDetectorState* state = static_cast<MotionDetectorState*>(detector);

    if (!prevBuffer || prevSize == 0)
        return false;

    // Walidacja rozmiaru - YUYV = width * height * 2
    size_t expectedSize = (size_t)(state->width * state->height * 2);
    if(currentSize != expectedSize || prevSize != expectedSize)
    {
        fprintf(stderr, "[MotionDetector] Zły rozmiar YUYV: current=%zu prev=%zu oczekiwano=%zu\n",
                currentSize, prevSize, expectedSize);
        return false;
    }

    Mat currentFrame(state->height, state->width, CV_8UC2, (void*)currentBuffer);
    Mat prevFrame   (state->height, state->width, CV_8UC2, (void*)prevBuffer);

    // konwersja do skali szarości
    Mat gray, prevGray;
    cvtColor(currentFrame, gray, COLOR_YUV2GRAY_YUYV);
    cvtColor(prevFrame, prevGray, COLOR_YUV2GRAY_YUYV);

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
    for(size_t i = 0; i < contours.size(); i++)
    {
        double area = contourArea(contours[i]);
        if(area > state->params.minArea)
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