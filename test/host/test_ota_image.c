/* OTA image header check (main/ota_image.c, identical in both repos): finding 3. Both
 * boards run this on the first OTA_IMAGE_HEADER_LEN bytes and call esp_ota_begin only when
 * it passes, so a refused image never reaches flash. */
#include <string.h>
#include "ota_image.h"
#include "t.h"

static void make_header(uint8_t *h, unsigned chip, uint32_t desc_magic, const char *project)
{
    memset(h, 0, OTA_IMAGE_HEADER_LEN);
    h[0] = OTA_IMAGE_MAGIC;
    h[OTA_IMAGE_CHIP_ID_OFFSET] = (uint8_t)chip;
    h[OTA_IMAGE_CHIP_ID_OFFSET + 1] = (uint8_t)(chip >> 8);
    for (int i = 0; i < 4; i++) h[OTA_IMAGE_DESC_OFFSET + i] = (uint8_t)(desc_magic >> (8 * i));
    strncpy((char *)&h[OTA_IMAGE_PROJECT_OFFSET], project, OTA_IMAGE_PROJECT_LEN);
}

static void test_own_project_accepted(void)
{
    uint8_t h[OTA_IMAGE_HEADER_LEN];
    make_header(h, OTA_IMAGE_CHIP_ESP32S3, OTA_IMAGE_DESC_MAGIC, "CarHvacController");
    CHECK(ota_image_check(h, sizeof(h), "CarHvacController", NULL, 0));
    make_header(h, OTA_IMAGE_CHIP_ESP32S3, OTA_IMAGE_DESC_MAGIC, "CarHvacIoNode");
    CHECK(ota_image_check(h, sizeof(h), "CarHvacIoNode", NULL, 0));
}

static void test_other_project_rejected(void)
{
    uint8_t h[OTA_IMAGE_HEADER_LEN];
    char why[96] = "";
    make_header(h, OTA_IMAGE_CHIP_ESP32S3, OTA_IMAGE_DESC_MAGIC, "CarHvacIoNode");
    CHECK(!ota_image_check(h, sizeof(h), "CarHvacController", why, sizeof(why)));
    CHECKM(strstr(why, "CarHvacIoNode") && strstr(why, "CarHvacController"), "why: %s", why);
    make_header(h, OTA_IMAGE_CHIP_ESP32S3, OTA_IMAGE_DESC_MAGIC, "CarHvacController");
    CHECK(!ota_image_check(h, sizeof(h), "CarHvacIoNode", why, sizeof(why)));
    make_header(h, OTA_IMAGE_CHIP_ESP32S3, OTA_IMAGE_DESC_MAGIC, "CarHvacControllerX");  /* no prefix match */
    CHECK(!ota_image_check(h, sizeof(h), "CarHvacController", why, sizeof(why)));
}

static void test_not_an_s3_app_rejected(void)
{
    uint8_t h[OTA_IMAGE_HEADER_LEN];
    char why[96];
    make_header(h, 0 /* ESP32 */, OTA_IMAGE_DESC_MAGIC, "CarHvacController");
    CHECK(!ota_image_check(h, sizeof(h), "CarHvacController", why, sizeof(why)));
    make_header(h, OTA_IMAGE_CHIP_ESP32S3, 0x12345678, "CarHvacController");
    CHECK(!ota_image_check(h, sizeof(h), "CarHvacController", why, sizeof(why)));
    make_header(h, OTA_IMAGE_CHIP_ESP32S3, OTA_IMAGE_DESC_MAGIC, "CarHvacController");
    h[0] = 0x7F;                                   /* e.g. an ELF or a text file */
    CHECK(!ota_image_check(h, sizeof(h), "CarHvacController", why, sizeof(why)));
    make_header(h, OTA_IMAGE_CHIP_ESP32S3, OTA_IMAGE_DESC_MAGIC, "CarHvacController");
    CHECK(!ota_image_check(h, OTA_IMAGE_HEADER_LEN - 1, "CarHvacController", why, sizeof(why)));
}

/* A 32-character name has no NUL in the descriptor: compared without reading past it. */
static void test_full_length_name(void)
{
    uint8_t h[OTA_IMAGE_HEADER_LEN + 1];
    make_header(h, OTA_IMAGE_CHIP_ESP32S3, OTA_IMAGE_DESC_MAGIC, "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345");
    h[OTA_IMAGE_PROJECT_OFFSET + OTA_IMAGE_PROJECT_LEN] = 'X';
    CHECK(ota_image_check(h, OTA_IMAGE_HEADER_LEN, "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", NULL, 0));
}

int main(void)
{
    RUN(test_own_project_accepted);
    RUN(test_other_project_rejected);
    RUN(test_not_an_s3_app_rejected);
    RUN(test_full_length_name);
    T_DONE();
}
