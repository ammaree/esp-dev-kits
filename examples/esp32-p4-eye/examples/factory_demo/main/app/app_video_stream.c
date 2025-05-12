/**
 * @file app_video_stream.c
 * @brief Video streaming application implementation
 *
 * This module provides functionality for initializing and managing video streaming
 * from a camera to a display. It handles camera initialization, buffer management,
 * and frame processing with scaling capabilities.
 */

#include <sys/stat.h> 
#include <dirent.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "driver/ppa.h"
#include "driver/jpeg_encode.h"
#include "bsp/esp-bsp.h"

#include "ui_extra.h"
#include "app_video.h"
#include "app_storage.h"
#include "app_isp.h"
#include "app_video_stream.h"
#include "app_video_utils.h"
#include "app_video_photo.h"
#include "app_video_record.h"

/* Constants */
#define ALIGN_UP(num, align)    (((num) + ((align) - 1)) & ~((align) - 1))
#define JPEG_COMPRESSION_RATIO  5             // Assuming 5:1 compression ratio
#define CAMERA_INIT_FRAMES      50            // Number of frames needed for camera initialization

static const char *TAG = "app_video_stream";

/* Type definitions */
/**
 * @brief Camera state structure
 */
typedef struct {
    struct {
        unsigned int is_initialized : 1;
        unsigned int is_take_photo : 1;
        unsigned int is_take_video : 1;
        unsigned int is_interval_photo_active : 1;
        unsigned int is_flash_light_on : 1;
        unsigned int reserved : 27;  // 保留位，用于未来扩展
    } flags;
    uint32_t init_count;
    uint16_t current_interval_minutes;
    uint32_t next_wake_time;
} camera_state_t;

/**
 * @brief Camera buffer management structure
 */
typedef struct {
    void *canvas_buf[EXAMPLE_CAM_BUF_NUM];
    uint8_t *photo_buf;
    uint8_t *jpg_buf;
    uint32_t jpg_size;
    size_t rx_buffer_size;
    uint8_t *scaled_camera_buf;
    int video_cam_fd;
} camera_buffer_t;

/* Static variables */
static camera_state_t camera_state = {
    .flags.is_initialized = false,
    .flags.is_take_photo = false,
    .flags.is_take_video = false,
    .flags.is_interval_photo_active = false,
    .flags.is_flash_light_on = false,
    .init_count = 0,
    .current_interval_minutes = 0,
    .next_wake_time = 0,
};

static camera_buffer_t camera_buffer = {
    .video_cam_fd = -1
};

static size_t data_cache_line_size = 0;

/* Forward declarations */
static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, 
                                        uint32_t camera_buf_hes, uint32_t camera_buf_ves, 
                                        size_t camera_buf_len);

/* Public function implementations */

/**
 * @brief Set photo resolution by string
 * 
 * @param resolution_str Resolution string ("480P", "720P", "1080P")
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_video_stream_set_photo_resolution_by_string(const char *resolution_str)
{
    if (strcmp(resolution_str, "480P") == 0 || strcmp(resolution_str, "480p") == 0) {
        app_video_stream_set_photo_resolution(PHOTO_RESOLUTION_480P);
        app_video_stream_set_video_resolution(PHOTO_RESOLUTION_480P);
    } else if (strcmp(resolution_str, "720P") == 0 || strcmp(resolution_str, "720p") == 0) {
        app_video_stream_set_photo_resolution(PHOTO_RESOLUTION_720P);
        app_video_stream_set_video_resolution(PHOTO_RESOLUTION_720P);
    } else if (strcmp(resolution_str, "1080P") == 0 || strcmp(resolution_str, "1080p") == 0) {
        app_video_stream_set_photo_resolution(PHOTO_RESOLUTION_1080P);
        app_video_stream_set_video_resolution(PHOTO_RESOLUTION_1080P);
    } else {
        ESP_LOGW(TAG, "Unknown resolution string: %s, using default 1080P", resolution_str);
        app_video_stream_set_photo_resolution(PHOTO_RESOLUTION_1080P);
        app_video_stream_set_video_resolution(PHOTO_RESOLUTION_1080P);
    }

    return ESP_OK;
}

/**
 * @brief Set flash light state
 * 
 * @param is_on Whether to turn flash on or off
 * @return ESP_OK on success
 */
esp_err_t app_video_stream_set_flash_light(bool is_on)
{
    camera_state.flags.is_flash_light_on = is_on;
    return ESP_OK;
}

/**
 * @brief Trigger photo capture
 * 
 * @return ESP_OK on success
 */
esp_err_t app_video_stream_take_photo(void)
{
    camera_state.flags.is_take_photo = true;
    return ESP_OK;
}

/**
 * @brief Stop photo capture
 * 
 * @return ESP_OK on success
 */
esp_err_t app_video_stream_stop_take_photo(void)
{
    camera_state.flags.is_take_photo = false;
    return ESP_OK;
}

/**
 * @brief Start video recording
 * 
 * @return ESP_OK on success
 */
esp_err_t app_video_stream_take_video(void)
{
    camera_state.flags.is_take_video = true;
    app_video_stream_start_recording();
    return ESP_OK;
}

/**
 * @brief Stop video recording
 * 
 * @return ESP_OK on success
 */
esp_err_t app_video_stream_stop_take_video(void)
{
    camera_state.flags.is_take_video = false;
    app_video_stream_stop_recording();
    return ESP_OK;
}

/**
 * @brief Stop interval photo capture
 * 
 * @return ESP_OK on success
 */
esp_err_t app_video_stream_stop_interval_photo(void)
{
    camera_state.flags.is_interval_photo_active = false;
    
    // Save interval photo state (closed)
    app_storage_save_interval_state(false, 0);

    app_storage_save_photo_count(app_extra_get_saved_photo_count());
    
    ESP_LOGI(TAG, "Interval photo stopped");
    
    return ESP_OK;
}

/**
 * @brief Start interval photo capture
 * 
 * @param interval_minutes Interval between photos in minutes
 * @return ESP_OK on success
 */
esp_err_t app_video_stream_start_interval_photo(uint16_t interval_minutes)
{
    camera_state.current_interval_minutes = interval_minutes;
    camera_state.flags.is_interval_photo_active = true;
    
    // Save interval photo state
    camera_state.next_wake_time = esp_timer_get_time() / 1000000 + interval_minutes * 60;
    app_storage_save_interval_state(true, camera_state.next_wake_time);
    
    // Take a photo immediately
    app_video_stream_take_photo();
    ESP_LOGI(TAG, "Interval photo started with interval %d minutes", interval_minutes);
    
    return ESP_OK;
}

/**
 * @brief Get scaled camera buffer
 * 
 * @param buf Pointer to store buffer address
 * @param size Pointer to store buffer size
 */
void app_video_stream_get_scaled_camera_buf(uint8_t **buf, uint32_t *size)
{
    *buf = camera_buffer.scaled_camera_buf;
    *size = ALIGN_UP(1920 * 1080 * 2, data_cache_line_size);
}

/**
 * @brief Get JPEG buffer
 * 
 * @param buf Pointer to store buffer address
 * @param size Pointer to store buffer size
 */
void app_video_stream_get_jpg_buf(uint8_t **buf, uint32_t *size)
{
    *buf = camera_buffer.jpg_buf;
    *size = camera_buffer.rx_buffer_size;
}

/**
 * @brief Get flash light state
 * 
 * @return true if flash light is on, false otherwise
 */
bool app_video_stream_get_flash_light_state(void)
{
    return camera_state.flags.is_flash_light_on;
}

/**
 * @brief Get interval photo state
 * 
 * @return true if interval photo is active, false otherwise
 */
bool app_video_stream_get_interval_photo_state(void)
{
    return camera_state.flags.is_interval_photo_active;
}

/**
 * @brief Get current interval minutes
 * 
 * @return Current interval in minutes
 */
uint16_t app_video_stream_get_current_interval_minutes(void)
{
    return camera_state.current_interval_minutes;
}

/**
 * @brief Get video file descriptor
 * 
 * @return Video camera file descriptor
 */
int app_video_stream_get_video_fd(void)
{
    return camera_buffer.video_cam_fd;
}

/**
 * @brief Initialize video streaming
 * 
 * @param i2c_handle I2C master bus handle for camera communication
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_video_stream_init(i2c_master_bus_handle_t i2c_handle)
{
    esp_err_t ret = ESP_OK;
    bool resources_initialized = false;
    uint32_t loaded_contrast = DEFAULT_CONTRAST_PERCENT;
    uint32_t loaded_saturation = DEFAULT_SATURATION_PERCENT;
    uint32_t loaded_brightness = DEFAULT_BRIGHTNESS_PERCENT;
    uint32_t loaded_hue = DEFAULT_HUE_PERCENT;
    uint16_t saved_count = 0;

    // Initialize video utils first
    ret = app_video_utils_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize video utils: 0x%x", ret);
        return ret;
    }
    
    // Get cache alignment
    ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get cache alignment: 0x%x", ret);
        goto cleanup;
    }

    // Initialize video camera
    ret = app_video_main(i2c_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Video main init failed with error 0x%x", ret);
        goto cleanup;
    }

    // Open video device
    camera_buffer.video_cam_fd = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT);
    if (camera_buffer.video_cam_fd < 0) {
        ESP_LOGE(TAG, "Video cam open failed");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // Initialize ISP
    ret = app_isp_init(camera_buffer.video_cam_fd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ISP: 0x%x", ret);
        goto cleanup;
    }

    // Load camera settings
    ret = app_storage_load_camera_settings(&loaded_contrast, &loaded_saturation, 
                                          &loaded_brightness, &loaded_hue);
    if (ret == ESP_OK) {
        // Apply camera settings
        app_isp_set_contrast(loaded_contrast);
        app_isp_set_saturation(loaded_saturation);
        app_isp_set_brightness(loaded_brightness);
        app_isp_set_hue(loaded_hue);
    }

    // Allocate canvas buffers
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        camera_buffer.canvas_buf[i] = heap_caps_aligned_calloc(data_cache_line_size, 1, 
                                                             BSP_LCD_H_RES * BSP_LCD_V_RES * 2, 
                                                             MALLOC_CAP_SPIRAM);
        if (camera_buffer.canvas_buf[i] == NULL) {
            ESP_LOGE(TAG, "Failed to allocate canvas buffer %d", i);
            ret = ESP_FAIL;
            goto cleanup;
        }
    }

    // Allocate scaled camera buffer
    camera_buffer.scaled_camera_buf = heap_caps_aligned_calloc(data_cache_line_size, 1, 1920 * 1080 * 2, MALLOC_CAP_SPIRAM);
    if (camera_buffer.scaled_camera_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate adjusted camera buffer");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // Allocate JPEG buffer
    jpeg_encode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    camera_buffer.jpg_buf = (uint8_t*)jpeg_alloc_encoder_mem(
        1920 * 1088 * 2 / JPEG_COMPRESSION_RATIO, 
        &rx_mem_cfg, 
        &camera_buffer.rx_buffer_size
    );
    if (camera_buffer.jpg_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JPEG buffer");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // Initialize video capture device
    ret = app_video_set_bufs(camera_buffer.video_cam_fd, EXAMPLE_CAM_BUF_NUM, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set video buffers: 0x%x", ret);
        goto cleanup;
    }

    // Register video frame operation callback
    ret = app_video_register_frame_operation_cb(camera_video_frame_operation);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register frame operation callback: 0x%x", ret);
        goto cleanup;
    }

    // Load saved photo count
    if (app_storage_get_photo_count(&saved_count) == ESP_OK) {
        app_extra_set_saved_photo_count(saved_count);
    }

    // Initialize video photo
    ret = app_video_photo_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize video photo: 0x%x", ret);
        goto cleanup;
    }

    // Initialize video record
    ret = app_video_record_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize video record: 0x%x", ret);
        goto cleanup;
    }

    // Start camera stream task
    ret = app_video_stream_task_start(camera_buffer.video_cam_fd, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start video stream task: 0x%x", ret);
        goto cleanup;
    }

    resources_initialized = true;

cleanup:
    if (!resources_initialized) {
        // Clean up resources in reverse order of initialization
        if (camera_buffer.scaled_camera_buf != NULL) {
            heap_caps_free(camera_buffer.scaled_camera_buf);
            camera_buffer.scaled_camera_buf = NULL;
        }
        
        for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
            if (camera_buffer.canvas_buf[i] != NULL) {
                heap_caps_free(camera_buffer.canvas_buf[i]);
                camera_buffer.canvas_buf[i] = NULL;
            }
        }
        
        if (camera_buffer.video_cam_fd >= 0) {
            app_video_close(camera_buffer.video_cam_fd);
        }
        
        app_video_utils_deinit();
    }

    return ret;
}

/* Private function implementations */

/**
 * @brief Process camera video frame
 * 
 * @param camera_buf Camera buffer containing the frame
 * @param camera_buf_index Buffer index
 * @param camera_buf_hes Horizontal resolution
 * @param camera_buf_ves Vertical resolution
 * @param camera_buf_len Buffer length
 */
static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, 
                                        uint32_t camera_buf_hes, uint32_t camera_buf_ves, 
                                        size_t camera_buf_len)
{
    int scale_level = app_extra_get_magnification_factor();

    // Camera initialization check
    if(!camera_state.flags.is_initialized) {
        camera_state.init_count++;
        if(camera_state.init_count >= CAMERA_INIT_FRAMES) {
            camera_state.flags.is_initialized = true;
            camera_state.init_count = 0;
            ESP_LOGI(TAG, "Camera initialized after %d frames", CAMERA_INIT_FRAMES);
        }
    }

    esp_err_t ret = app_image_process_video_frame(
        camera_buf, camera_buf_hes, camera_buf_ves,
        scale_level,
        camera_buffer.canvas_buf[camera_buf_index], 
        ALIGN_UP(BSP_LCD_H_RES * BSP_LCD_V_RES * 2, data_cache_line_size)
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to process frame: 0x%x", ret);
        return;
    }

    // Swap RGB565 bytes
    swap_rgb565_bytes(camera_buffer.canvas_buf[camera_buf_index], BSP_LCD_H_RES * BSP_LCD_V_RES);

    // Update display
    bsp_display_lock(0);
    lv_canvas_set_buffer(ui_PanelCanvas, camera_buffer.canvas_buf[camera_buf_index], BSP_LCD_H_RES, BSP_LCD_V_RES, LV_IMG_CF_TRUE_COLOR);
    lv_refr_now(NULL);
    bsp_display_unlock();

    // Handle photo request
    if (camera_state.flags.is_initialized) {
        // Handle photo request
        if (camera_state.flags.is_take_photo && 
            (ui_extra_get_current_page() == UI_PAGE_CAMERA || ui_extra_get_current_page() == UI_PAGE_INTERVAL_CAM)) {
            // Reset photo flag and take a photo
            camera_state.flags.is_take_photo = false;
            take_and_save_photo(camera_buf, camera_buf_hes, camera_buf_ves);
        } 
        // Handle video request
        else if (camera_state.flags.is_take_video && ui_extra_get_current_page() == UI_PAGE_VIDEO_MODE) {
            take_and_save_video(camera_buf, camera_buf_hes, camera_buf_ves);
        }
    }
}