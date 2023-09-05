#ifdef USE_ESP32

#include "esp32_camera.h"
#include "usb_stream.h"

#include "esphome/core/log.h"

#include <freertos/event_groups.h>
#include <freertos/task.h>


static const char *const TAG = "usb_webcam";

#define BIT0_FRAME_START     (0x01 << 0)
#define BIT1_NEW_FRAME_START (0x01 << 1)
#define BIT2_NEW_FRAME_END   (0x01 << 2)
#define BIT3_SPK_START       (0x01 << 3)
#define BIT4_SPK_RESET       (0x01 << 4)

static EventGroupHandle_t s_evt_handle;

#ifdef CONFIG_IDF_TARGET_ESP32S2
#define UVC_XFER_BUFFER_SIZE (45 * 1024)
#else
#define UVC_XFER_BUFFER_SIZE (55 * 1024)
#endif
static camera_fb_t s_fb;

camera_fb_t *esp_camera_fb_get()
{
    xEventGroupSetBits(s_evt_handle, BIT0_FRAME_START);
    xEventGroupWaitBits(s_evt_handle, BIT1_NEW_FRAME_START, true, true, portMAX_DELAY);
    return &s_fb;
}

void esp_camera_fb_return(camera_fb_t *fb)
{
    xEventGroupSetBits(s_evt_handle, BIT2_NEW_FRAME_END);
    return;
}

namespace esphome {
namespace esp32_camera {

static void camera_frame_cb(uvc_frame_t *frame, void *ptr)
{
    ESP_LOGD(TAG, "uvc frame format = %d, seq = %u, width = %u, height = %u, length = %u",
             frame->frame_format, frame->sequence, frame->width, frame->height, frame->data_bytes);
    if (!(xEventGroupGetBits(s_evt_handle) & BIT0_FRAME_START)) {
        return;
    }

    switch (frame->frame_format) {
    case UVC_FRAME_FORMAT_MJPEG:
        s_fb.buf = (uint8_t*)frame->data;
        s_fb.len = frame->data_bytes;
        s_fb.width = frame->width;
        s_fb.height = frame->height;
        s_fb.buf = (uint8_t*)frame->data;
        s_fb.format = PIXFORMAT_JPEG;
        s_fb.timestamp.tv_sec = frame->sequence;
        xEventGroupSetBits(s_evt_handle, BIT1_NEW_FRAME_START);
        ESP_LOGV(TAG, "send frame = %u", frame->sequence);
        xEventGroupWaitBits(s_evt_handle, BIT2_NEW_FRAME_END, true, true, portMAX_DELAY);
        ESP_LOGV(TAG, "send frame done = %u", frame->sequence);
        break;
    default:
        ESP_LOGW(TAG, "Format not supported");
        assert(0);
        break;
    }
}

static void stream_state_changed_cb(usb_stream_state_t event, void *arg)
{
    switch (event) {
    case STREAM_CONNECTED: {
        size_t frame_size = 0;
        size_t frame_index = 0;
        uvc_frame_size_list_get(NULL, &frame_size, &frame_index);
        if (frame_size) {
            ESP_LOGI(TAG, "UVC: get frame list size = %u, current = %u", frame_size, frame_index);
            uvc_frame_size_t *uvc_frame_list = (uvc_frame_size_t *)malloc(frame_size * sizeof(uvc_frame_size_t));
            uvc_frame_size_list_get(uvc_frame_list, NULL, NULL);
            for (size_t i = 0; i < frame_size; i++) {
                ESP_LOGI(TAG, "\tframe[%u] = %ux%u", i, uvc_frame_list[i].width, uvc_frame_list[i].height);
            }
            free(uvc_frame_list);
        } else {
            ESP_LOGW(TAG, "UVC: get frame list size = %u", frame_size);
        }
        ESP_LOGI(TAG, "Device connected");
        break;
    }
    case STREAM_DISCONNECTED:
        ESP_LOGI(TAG, "Device disconnected");
        break;
    default:
        ESP_LOGE(TAG, "Unknown event");
        break;
    }
}

esp_err_t esp_camera_init(ESP32CameraFrameSize fs) {
  memset(&s_fb, 0, sizeof(camera_fb_t));
  s_evt_handle = xEventGroupCreate();
  if (s_evt_handle == NULL) {
      ESP_LOGE(TAG, "Event group create failed");
      assert(0);
  }
  /* malloc double buffer for usb payload, xfer_buffer_size >= frame_buffer_size*/
  uint8_t *xfer_buffer_a = (uint8_t *)malloc(UVC_XFER_BUFFER_SIZE);
  assert(xfer_buffer_a != NULL);
  uint8_t *xfer_buffer_b = (uint8_t *)malloc(UVC_XFER_BUFFER_SIZE);
  assert(xfer_buffer_b != NULL);
  /* malloc frame buffer for a jpeg frame*/
  uint8_t *frame_buffer = (uint8_t *)malloc(UVC_XFER_BUFFER_SIZE);
  assert(frame_buffer != NULL);

  uvc_config_t uvc_config = {
      .frame_width = 0,
      .frame_height = 0,
      .frame_interval = FPS2INTERVAL(15),
      .xfer_buffer_size = UVC_XFER_BUFFER_SIZE,
      .xfer_buffer_a = xfer_buffer_a,
      .xfer_buffer_b = xfer_buffer_b,
      .frame_buffer_size = UVC_XFER_BUFFER_SIZE,
      .frame_buffer = frame_buffer,
      .frame_cb = &camera_frame_cb,
      .frame_cb_arg = NULL,
      .xfer_type = UVC_XFER_ISOC,
      .format_index = 0,
      .frame_index = 0,
      .interface = 1,
      .interface_alt = 1,
      .ep_addr = 0x83,
      .ep_mps = 512,
      .flags = 0
  };

  switch (fs) {
    case ESP32_CAMERA_SIZE_160X120:   uvc_config.frame_width = 160;  uvc_config.frame_height = 120; break;
    case ESP32_CAMERA_SIZE_176X144:   uvc_config.frame_width = 176;  uvc_config.frame_height = 144; break;
    case ESP32_CAMERA_SIZE_240X176:   uvc_config.frame_width = 240;  uvc_config.frame_height = 176; break;
    case ESP32_CAMERA_SIZE_320X240:   uvc_config.frame_width = 320;  uvc_config.frame_height = 240; break;
    case ESP32_CAMERA_SIZE_400X296:   uvc_config.frame_width = 400;  uvc_config.frame_height = 296; break;
    case ESP32_CAMERA_SIZE_640X480:   uvc_config.frame_width = 640;  uvc_config.frame_height = 480; break;
    case ESP32_CAMERA_SIZE_800X600:   uvc_config.frame_width = 800;  uvc_config.frame_height = 600; break;
    case ESP32_CAMERA_SIZE_1024X768:  uvc_config.frame_width = 1024; uvc_config.frame_height = 768; break;
    case ESP32_CAMERA_SIZE_1280X1024: uvc_config.frame_width = 1280; uvc_config.frame_height = 1024; break;
    case ESP32_CAMERA_SIZE_1600X1200: uvc_config.frame_width = 1600; uvc_config.frame_height = 1200; break;
    case ESP32_CAMERA_SIZE_1920X1080: uvc_config.frame_width = 1920; uvc_config.frame_height = 1080; break;
    case ESP32_CAMERA_SIZE_720X1280:  uvc_config.frame_width = 720;  uvc_config.frame_height = 1280; break;
    case ESP32_CAMERA_SIZE_864X1536:  uvc_config.frame_width = 864;  uvc_config.frame_height = 1536; break;
    case ESP32_CAMERA_SIZE_2048X1536: uvc_config.frame_width = 2048; uvc_config.frame_height = 1536; break;
    case ESP32_CAMERA_SIZE_2560X1440: uvc_config.frame_width = 2560; uvc_config.frame_height = 1440; break;
    case ESP32_CAMERA_SIZE_2560X1600: uvc_config.frame_width = 2560; uvc_config.frame_height = 1600; break;
    case ESP32_CAMERA_SIZE_1080X1920: uvc_config.frame_width = 1080; uvc_config.frame_height = 1920; break;
    case ESP32_CAMERA_SIZE_2560X1920: uvc_config.frame_width = 2560; uvc_config.frame_height = 1920; break;
    default: return ESP_ERR_INVALID_ARG;
  }
  /* config to enable uvc function */
  esp_err_t ret = uvc_streaming_config(&uvc_config);
  if (ret != ESP_OK) {
      ESP_LOGE(TAG, "uvc streaming config failed");
      return ret;
  }
  /* register the state callback to get connect/disconnect event 
  * in the callback, we can get the frame list of current device
  */
  ret = usb_streaming_state_register(&stream_state_changed_cb, NULL);
  if (ret != ESP_OK) return ret;
  /* start usb streaming, UVC and UAC MIC will start streaming because SUSPEND_AFTER_START flags not set */
  ret = usb_streaming_start();
  #if WAIT_FOR_USB_CONNECT
  if (ret != ESP_OK) return ret;
  ret = usb_streaming_connect_wait(portMAX_DELAY);
  #endif
  return ret;
}

/* ---------------- public API (derivated) ---------------- */
void ESP32Camera::setup() {
  //esp_log_level_set(TAG, ESP_LOG_DEBUG);
  global_esp32_camera = this;

  /* initialize time to now */
  this->last_update_ = esp_timer_get_time();

  /* initialize camera */
  esp_err_t err = esp_camera_init(this->frame_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
    this->init_error_ = err;
    this->mark_failed();
    return;
  }

  /* initialize camera parameters */
  this->update_camera_parameters();

  /* initialize RTOS */
  this->framebuffer_get_queue_ = xQueueCreate(1, sizeof(camera_fb_t *));
  this->framebuffer_return_queue_ = xQueueCreate(1, sizeof(camera_fb_t *));
  xTaskCreatePinnedToCore(&ESP32Camera::framebuffer_task,
                          "framebuffer_task",  // name
                          1024,                // stack size
                          nullptr,             // task pv params
                          0,                   // priority
                          nullptr,             // handle
                          1                    // core
  );
}

void ESP32Camera::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP32 USB WebCamera:");
  ESP_LOGCONFIG(TAG, "  Name: %s", this->name_.c_str());
  switch (frame_size) {
    case ESP32_CAMERA_SIZE_160X120:
      ESP_LOGCONFIG(TAG, "  Resolution: 160x120 (QQVGA)");
      break;
    case ESP32_CAMERA_SIZE_176X144:
      ESP_LOGCONFIG(TAG, "  Resolution: 176x144 (QCIF)");
      break;
    case ESP32_CAMERA_SIZE_240X176:
      ESP_LOGCONFIG(TAG, "  Resolution: 240x176 (HQVGA)");
      break;
    case ESP32_CAMERA_SIZE_320X240:
      ESP_LOGCONFIG(TAG, "  Resolution: 320x240 (QVGA)");
      break;
    case ESP32_CAMERA_SIZE_400X296:
      ESP_LOGCONFIG(TAG, "  Resolution: 400x296 (CIF)");
      break;
    case ESP32_CAMERA_SIZE_640X480:
      ESP_LOGCONFIG(TAG, "  Resolution: 640x480 (VGA)");
      break;
    case ESP32_CAMERA_SIZE_800X600:
      ESP_LOGCONFIG(TAG, "  Resolution: 800x600 (SVGA)");
      break;
    case ESP32_CAMERA_SIZE_1024X768:
      ESP_LOGCONFIG(TAG, "  Resolution: 1024x768 (XGA)");
      break;
    case ESP32_CAMERA_SIZE_1280X1024:
      ESP_LOGCONFIG(TAG, "  Resolution: 1280x1024 (SXGA)");
      break;
    case ESP32_CAMERA_SIZE_1600X1200:
      ESP_LOGCONFIG(TAG, "  Resolution: 1600x1200 (UXGA)");
      break;
    case ESP32_CAMERA_SIZE_1920X1080:
      ESP_LOGCONFIG(TAG, "  Resolution: 1920x1080 (FHD)");
      break;
    case ESP32_CAMERA_SIZE_720X1280:
      ESP_LOGCONFIG(TAG, "  Resolution: 720x1280 (P_HD)");
      break;
    case ESP32_CAMERA_SIZE_864X1536:
      ESP_LOGCONFIG(TAG, "  Resolution: 864x1536 (P_3MP)");
      break;
    case ESP32_CAMERA_SIZE_2048X1536:
      ESP_LOGCONFIG(TAG, "  Resolution: 2048x1536 (QXGA)");
      break;
    case ESP32_CAMERA_SIZE_2560X1440:
      ESP_LOGCONFIG(TAG, "  Resolution: 2560x1440 (QHD)");
      break;
    case ESP32_CAMERA_SIZE_2560X1600:
      ESP_LOGCONFIG(TAG, "  Resolution: 2560x1600 (WQXGA)");
      break;
    case ESP32_CAMERA_SIZE_1080X1920:
      ESP_LOGCONFIG(TAG, "  Resolution: 1080x1920 (P_FHD)");
      break;
    case ESP32_CAMERA_SIZE_2560X1920:
      ESP_LOGCONFIG(TAG, "  Resolution: 2560x1920 (QSXGA)");
      break;
  }

  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup Failed: %s", esp_err_to_name(this->init_error_));
    return;
  }
}

void ESP32Camera::loop() {
  // check if we can return the image
  if (this->can_return_image_()) {
    // return image
    auto *fb = this->current_image_->get_raw_buffer();
    xQueueSend(this->framebuffer_return_queue_, &fb, portMAX_DELAY);
    this->current_image_.reset();
  }

  // request idle image every idle_update_interval
  const uint64_t now = esp_timer_get_time();
  if (this->idle_update_interval_ != 0 && now - this->last_idle_request_ > this->idle_update_interval_) {
    this->last_idle_request_ = now;
    this->request_image(IDLE);
  }

  // Check if we should fetch a new image
  if (!this->has_requested_image_())
    return;
  if (this->current_image_.use_count() > 1) {
    // image is still in use
    return;
  }
  if (now - this->last_update_ <= this->max_update_interval_)
    return;

  // request new image
  camera_fb_t *fb;
  if (xQueueReceive(this->framebuffer_get_queue_, &fb, 0L) != pdTRUE) {
    // no frame ready
    ESP_LOGVV(TAG, "No frame ready");
    return;
  }

  if (fb == nullptr) {
    ESP_LOGW(TAG, "Got invalid frame from camera!");
    xQueueSend(this->framebuffer_return_queue_, &fb, portMAX_DELAY);
    return;
  }
  this->current_image_ = std::make_shared<CameraImage>(fb, this->single_requesters_ | this->stream_requesters_);

  ESP_LOGV(TAG, "Got Image: len=%u", fb->len);
  this->new_image_callback_.call(this->current_image_);
  this->last_update_ = now;
  this->single_requesters_ = 0;
}

float ESP32Camera::get_setup_priority() const { return setup_priority::DATA; }

/* ---------------- constructors ---------------- */
ESP32Camera::ESP32Camera() {
  frame_size = ESP32_CAMERA_SIZE_640X480;
  global_esp32_camera = this;
}

/* ---------------- setters ---------------- */

/* set image parameters */
void ESP32Camera::set_frame_size(ESP32CameraFrameSize size) {
  this->frame_size = size;
}
/* set fps */
void ESP32Camera::set_max_update_interval(uint32_t max_update_interval) {
  this->max_update_interval_ = max_update_interval;
}
void ESP32Camera::set_idle_update_interval(uint32_t idle_update_interval) {
  this->idle_update_interval_ = idle_update_interval;
}

/* ---------------- public API (specific) ---------------- */
void ESP32Camera::add_image_callback(std::function<void(std::shared_ptr<CameraImage>)> &&f) {
  this->new_image_callback_.add(std::move(f));
}
void ESP32Camera::add_stream_start_callback(std::function<void()> &&callback) {
  this->stream_start_callback_.add(std::move(callback));
}
void ESP32Camera::add_stream_stop_callback(std::function<void()> &&callback) {
  this->stream_stop_callback_.add(std::move(callback));
}
void ESP32Camera::start_stream(CameraRequester requester) {
  this->stream_start_callback_.call();
  this->stream_requesters_ |= (1U << requester);
}
void ESP32Camera::stop_stream(CameraRequester requester) {
  this->stream_stop_callback_.call();
  this->stream_requesters_ &= ~(1U << requester);
}
void ESP32Camera::request_image(CameraRequester requester) { this->single_requesters_ |= (1U << requester); }
void ESP32Camera::update_camera_parameters() {
}

/* ---------------- Internal methods ---------------- */
bool ESP32Camera::has_requested_image_() const { return this->single_requesters_ || this->stream_requesters_; }
bool ESP32Camera::can_return_image_() const { return this->current_image_.use_count() == 1; }


void ESP32Camera::framebuffer_task(void *pv) {
  while (true) {
    camera_fb_t *framebuffer = esp_camera_fb_get();
    xQueueSend(global_esp32_camera->framebuffer_get_queue_, &framebuffer, portMAX_DELAY);
    // return is no-op for config with 1 fb
    xQueueReceive(global_esp32_camera->framebuffer_return_queue_, &framebuffer, portMAX_DELAY);
    esp_camera_fb_return(framebuffer);
  }
}

ESP32Camera *global_esp32_camera;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/* ---------------- CameraImageReader class ---------------- */
void CameraImageReader::set_image(std::shared_ptr<CameraImage> image) {
  this->image_ = std::move(image);
  this->offset_ = 0;
}
size_t CameraImageReader::available() const {
  if (!this->image_)
    return 0;

  return this->image_->get_data_length() - this->offset_;
}
void CameraImageReader::return_image() { this->image_.reset(); }
void CameraImageReader::consume_data(size_t consumed) { this->offset_ += consumed; }
uint8_t *CameraImageReader::peek_data_buffer() { return this->image_->get_data_buffer() + this->offset_; }

/* ---------------- CameraImage class ---------------- */
CameraImage::CameraImage(camera_fb_t *buffer, uint8_t requesters) : buffer_(buffer), requesters_(requesters) {}

camera_fb_t *CameraImage::get_raw_buffer() { return this->buffer_; }
uint8_t *CameraImage::get_data_buffer() { return this->buffer_->buf; }
size_t CameraImage::get_data_length() { return this->buffer_->len; }
bool CameraImage::was_requested_by(CameraRequester requester) const {
  return (this->requesters_ & (1 << requester)) != 0;
}

}  // namespace esp32_camera
}  // namespace esphome

#endif
