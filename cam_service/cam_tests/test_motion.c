#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../motion_detector.h"

// Helper: wczytaj plik JPEG do bufora
typedef struct {
    unsigned char* data;
    size_t size;
} ImageBuffer;

static ImageBuffer load_jpeg_file(const char* filename) {
    ImageBuffer img = {NULL, 0};
    FILE* f = fopen(filename, "rb");
    
    if (!f) {
        fprintf(stderr, "Nie można otworzyć: %s\n", filename);
        return img;
    }
    
    fseek(f, 0, SEEK_END);
    img.size = ftell(f);
    rewind(f);
    
    img.data = malloc(img.size);
    if (img.data) {
        fread(img.data, 1, img.size, f);
    }
    
    fclose(f);
    return img;
}

static void free_image_buffer(ImageBuffer* img) {
    if (img && img->data) {
        free(img->data);
        img->data = NULL;
        img->size = 0;
    }
}

// Setup/Teardown dla każdego testu
static int setup(void **state) {
    MotionParams params = {
        .motionThreshold = 20,
        .minArea = 200,
        .gaussBlur = 21
    };
    
    void* detector = motion_detector_init(640, 480, params);
    assert_non_null(detector);
    
    *state = detector;
    return 0;
}

static int teardown(void **state) {
    void* detector = *state;
    motion_detector_destroy(detector);
    return 0;
}

// ============ TESTY ============

// Test 1: Brak ruchu - ten sam obraz dwukrotnie
static void test_no_motion_same_image(void **state) {
    void* detector = *state;
    
    ImageBuffer img = load_jpeg_file("images/move_1.jpg");
    assert_non_null(img.data);
    assert_true(img.size > 0);
    
    // Pierwsze wywołanie - inicjalizacja
    bool motion1 = motion_detector_detect(detector, 
        img.data, img.size, NULL, 0);
    assert_false(motion1); // Brak poprzedniej klatki = brak ruchu
    
    // Drugie wywołanie - ten sam obraz
    bool motion2 = motion_detector_detect(detector,
        img.data, img.size, img.data, img.size);
    assert_false(motion2); // Powinno być FALSE
    
    free_image_buffer(&img);
}

// Test 2: Wykrycie ruchu - różne obrazy
static void test_detects_motion_different_images(void **state) {
    void* detector = *state;
    
    ImageBuffer img1 = load_jpeg_file("images/move_1.jpg");
    ImageBuffer img2 = load_jpeg_file("images/move_2.jpg");
    
    assert_non_null(img1.data);
    assert_non_null(img2.data);
    
    // Inicjalizacja pierwszym obrazem
    motion_detector_detect(detector, img1.data, img1.size, NULL, 0);
    
    // Drugi obraz - powinien wykryć ruch
    bool motion = motion_detector_detect(detector,
        img2.data, img2.size, img1.data, img1.size);
    
    assert_true(motion); // Powinno być TRUE
    
    free_image_buffer(&img1);
    free_image_buffer(&img2);
}

// Test 3: Przemienne klatki A->B->A
static void test_alternating_frames(void **state) {
    void* detector = *state;
    
    ImageBuffer imgA = load_jpeg_file("images/move_1.jpg");
    ImageBuffer imgB = load_jpeg_file("images/move_2.jpg");
    
    assert_non_null(imgA.data);
    assert_non_null(imgB.data);
    
    // Inicjalizacja
    motion_detector_detect(detector, imgA.data, imgA.size, NULL, 0);
    
    // A -> B (powinien wykryć ruch)
    bool motion1 = motion_detector_detect(detector,
        imgB.data, imgB.size, imgA.data, imgA.size);
    assert_true(motion1);
    
    // B -> A (też powinien wykryć ruch)
    bool motion2 = motion_detector_detect(detector,
        imgA.data, imgA.size, imgB.data, imgB.size);
    assert_true(motion2);
    
    // A -> A (brak ruchu)
    bool motion3 = motion_detector_detect(detector,
        imgA.data, imgA.size, imgA.data, imgA.size);
    assert_false(motion3);
    
    free_image_buffer(&imgA);
    free_image_buffer(&imgB);
}

// Test 4: Nieprawidłowe dane wejściowe
static void test_invalid_input(void **state) {
    void* detector = *state;
    
    // NULL detector
    bool result1 = motion_detector_detect(NULL, 
        (unsigned char*)"test", 4, NULL, 0);
    assert_false(result1);
    
    // NULL current buffer
    bool result2 = motion_detector_detect(detector,
        NULL, 100, NULL, 0);
    assert_false(result2);
    
    // Zero size
    bool result3 = motion_detector_detect(detector,
        (unsigned char*)"test", 0, NULL, 0);
    assert_false(result3);
}

// Test 5: Sprawdź czy JPEG jest poprawnie dekodowany
static void test_jpeg_validation(void **state) {
    void* detector = *state;
    
    ImageBuffer img = load_jpeg_file("images/move_1.jpg");
    assert_non_null(img.data);
    
    // Sprawdź nagłówek JPEG (FF D8)
    assert_int_equal(img.data[0], 0xFF);
    assert_int_equal(img.data[1], 0xD8);
    
    // Powinna się zdekodować bez błędów
    bool motion = motion_detector_detect(detector,
        img.data, img.size, NULL, 0);
    
    // Nie powinno być errora (false to OK dla pierwszej klatki)
    assert_false(motion);
    
    free_image_buffer(&img);
}

// Test 6: Różne parametry detekcji
static void test_motion_parameters(void **state)
{
    (void)state;

    ImageBuffer img1 = load_jpeg_file("images/move_1.jpg");
    ImageBuffer img2 = load_jpeg_file("images/move_2.jpg");

    // 1. detektor: tresh 5; min area 50; blur 21
    MotionParams sensitive = {
        .motionThreshold = 5,
        .minArea = 50,
        .gaussBlur = 21
    };

    void* detector = motion_detector_init(640, 480, sensitive);
    assert_non_null(detector);

    // inicjalizacja
    motion_detector_detect(detector, img1.data, img1.size, NULL, 0);

    bool motion = motion_detector_detect(
        detector, img2.data, img2.size, img1.data, img1.size);

    printf("Czuły detektor: %s\n", motion ? "wykrył ruch" : "nie wykrył");
    assert_true(motion);
    motion_detector_destroy(detector);

    // 2. detektor: tresh 50; min area 500; blur 21
    MotionParams insensitive = {
        .motionThreshold = 50,
        .minArea = 500,
        .gaussBlur = 21
    };

    void* detector = motion_detector_init(640, 480, insensitive);
    assert_non_null(detector);

    motion_detector_detect(detector, img1.data, img1.size, NULL, 0);

    bool motion = motion_detector_detect(
        detector, img2.data, img2.size, img1.data, img1.size);

    // printf("Detekrtor 2: %s\n", motion ? "wykrył ruch" : "nie wykrył");
    assert_true(motion);
    motion_detector_destroy(detector);

    // 3. detektor standardowy: tresh 50; min area 200; blur 21
    MotionParams base = {
        .motionThreshold = 20,
        .minArea = 200,
        .gaussBlur = 21
    };

    void* detector = motion_detector_init(640, 480, base);
    assert_non_null(detector);

    motion_detector_detect(detector, img1.data, img1.size, NULL, 0);

    bool motion = motion_detector_detect(
        detector, img2.data, img2.size, img1.data, img1.size);

    // printf("Detektor 3: %s\n", motion ? "wykrył ruch" : "nie wykrył");
    assert_true(motion);
    motion_detector_destroy(detector);

    free_image_buffer(&img1);
    free_image_buffer(&img2);
}


// ============ MAIN ============

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_no_motion_same_image, setup, teardown),
        cmocka_unit_test_setup_teardown(test_detects_motion_different_images, setup, teardown),
        cmocka_unit_test_setup_teardown(test_alternating_frames, setup, teardown),
        cmocka_unit_test_setup_teardown(test_invalid_input, setup, teardown),
        cmocka_unit_test_setup_teardown(test_jpeg_validation, setup, teardown),
        cmocka_unit_test(test_motion_parameters),
    };
    
    return cmocka_run_group_tests(tests, NULL, NULL);
}