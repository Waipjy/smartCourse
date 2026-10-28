#include <Arduino.h>
#include "HandTracker.h"
#include "SD_Model.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "mmf2_module.h"
#include "module_vipnn.h"
#include "model_palm_detection.h"
#include "model_hand_landmark.h"
#include "siso_drv.h"
#include "avcodec.h"
#include "vfs.h"

extern int vipnn_control(void *p, int cmd, int arg);

#ifdef __cplusplus
}
#endif

#undef min
#undef max

#define LIMIT(x, lower, upper) \
    if (x < lower) \
        x = lower; \
    else if (x > upper) \
        x = upper;

nn_data_param_t HandTracker::palm_nn_roi = {0};
nn_data_param_t HandTracker::handlandmark_nn_roi = {0};

volatile uint32_t HandTracker::statCallbacks = 0;
volatile uint32_t HandTracker::statNotFound = 0;
volatile uint32_t HandTracker::statFixFail = 0;
volatile uint32_t HandTracker::frameSeq = 0;
volatile bool HandTracker::handFound = false;
float HandTracker::landU[HAND_POINTS] = {0};
float HandTracker::landV[HAND_POINTS] = {0};

HandTracker::HandTracker(void)
{
}

HandTracker::~HandTracker(void)
{
    end();
}

void HandTracker::configVideo(VideoSetting &config)
{
    palm_nn_roi.img.width = config._w;
    palm_nn_roi.img.height = config._h;
    palm_nn_roi.img.rgb = 0;
    palm_nn_roi.img.roi.xmin = 0;
    palm_nn_roi.img.roi.xmax = config._w;
    palm_nn_roi.img.roi.ymin = 0;
    palm_nn_roi.img.roi.ymax = config._h;
    palm_nn_roi.codec_type = AV_CODEC_ID_RGB888;

    handlandmark_nn_roi.img.width = 224;
    handlandmark_nn_roi.img.height = 224;
    handlandmark_nn_roi.img.rgb = 0;
    handlandmark_nn_roi.img.roi.xmin = 0;
    handlandmark_nn_roi.img.roi.xmax = 224;
    handlandmark_nn_roi.img.roi.ymin = 0;
    handlandmark_nn_roi.img.roi.ymax = 224;
    handlandmark_nn_roi.codec_type = AV_CODEC_ID_NN_RAW;
}

void HandTracker::configThreshold(float confidence_threshold, float nms_threshold)
{
    pd_confidence_thresh = confidence_threshold;
    pd_nms_thresh = nms_threshold;
}

void HandTracker::begin(void)
{
    // Palm Detection Configuration
    if (_p_mmf_context == NULL) {
        _p_mmf_context = mm_module_open(&vipnn_module);
    }
    if (_p_mmf_context == NULL) {
        printf("\r\n[ERROR] HandTracker palm init failed\n");
        return;
    }
    if ((palm_nn_roi.img.width == 0) || (palm_nn_roi.img.height == 0)) {
        printf("\r\n[ERROR] HandTracker video not configured\n");
        return;
    }

    if (_nntask != GESTURE_DETECTION) {
        while (1) {
            printf("\r\n[ERROR] Invalid NN task selected! Please check modelSelect() again\n");
            delay(5000);
        }
    }

    vipnn_control(_p_mmf_context->priv, CMD_VIPNN_SET_MODEL, (int)&palm_detection_fwfs);
    vipnn_control(_p_mmf_context->priv, CMD_VIPNN_SET_IN_PARAMS, (int)&palm_nn_roi);
    mm_module_ctrl(_p_mmf_context, CMD_VIPNN_SET_RES_SIZE, sizeof(palmdetect_res_t));    // result size
    mm_module_ctrl(_p_mmf_context, CMD_VIPNN_SET_RES_MAX_CNT, MAX_HAND_DETECT_NUM);      // result max count
    mm_module_ctrl(_p_mmf_context, MM_CMD_SET_DATAGROUP, MM_GROUP_START);
    vipnn_control(_p_mmf_context->priv, CMD_VIPNN_SET_CONFIDENCE_THRES, (int)&pd_confidence_thresh);
    vipnn_control(_p_mmf_context->priv, CMD_VIPNN_SET_NMS_THRES, (int)&pd_nms_thresh);
    mm_module_ctrl(_p_mmf_context, CMD_VIPNN_SET_OUTPUT, 1);    // enable module output
    mm_module_ctrl(_p_mmf_context, MM_CMD_SET_QUEUE_LEN, 1);
    mm_module_ctrl(_p_mmf_context, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_STATIC);
    vipnn_control(_p_mmf_context->priv, CMD_VIPNN_APPLY, 0);

    // Hand Landmark
    if (handlandmark_ctx == NULL) {
        handlandmark_ctx = mm_module_open(&vipnn_module);
    }
    if (handlandmark_ctx == NULL) {
        printf("\r\n[ERROR] HandTracker landmark init failed\n");
        return;
    }
    if (palm_hand_siso_ctx == NULL) {
        palm_hand_siso_ctx = (void *)sisoCreate();
    }
    if (palm_hand_siso_ctx == NULL) {
        printf("\r\n[ERROR] HandTracker SISO init failed\n");
        return;
    }

    vipnn_control(handlandmark_ctx->priv, CMD_VIPNN_SET_MODEL, (int)&hand_landmark_fwfs);
    mm_module_ctrl(handlandmark_ctx, CMD_VIPNN_SET_IN_PARAMS, (int)&handlandmark_nn_roi);
    vipnn_control(handlandmark_ctx->priv, CMD_VIPNN_SET_CASCADE, 2);                     // this module is cascade mode
    vipnn_control(handlandmark_ctx->priv, CMD_VIPNN_SET_OUTPUT, 1);                      // output
    mm_module_ctrl(handlandmark_ctx, CMD_VIPNN_SET_RES_SIZE, sizeof(handland_res_t));    // result size
    mm_module_ctrl(handlandmark_ctx, CMD_VIPNN_SET_RES_MAX_CNT, MAX_HAND_DETECT_NUM);    // result max count

    mm_module_ctrl(handlandmark_ctx, MM_CMD_SET_DATAGROUP, MM_GROUP_END);
    mm_module_ctrl(handlandmark_ctx, MM_CMD_SET_QUEUE_LEN, 1);
    mm_module_ctrl(handlandmark_ctx, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_STATIC);
    mm_module_ctrl(handlandmark_ctx, CMD_VIPNN_SET_DISPPOST, (int)onResult);    // 核心这里挂的是画 OSD 的函式，换成存关键点
    vipnn_control(handlandmark_ctx->priv, CMD_VIPNN_APPLY, 0);

    // SISO link palm detection to HandLandmark
    sisoRegIn(palm_hand_siso_ctx, _p_mmf_context);
    sisoRegOut(palm_hand_siso_ctx, handlandmark_ctx);
    sisoStart(palm_hand_siso_ctx);
}

void HandTracker::end(void)
{
    if (palm_hand_siso_ctx == NULL) {
        return;
    }
    sisoDestroy(palm_hand_siso_ctx);
    palm_hand_siso_ctx = NULL;

    if (_p_mmf_context != NULL) {
        if (mm_module_close(_p_mmf_context) == NULL) {
            _p_mmf_context = NULL;
        } else {
            printf("\r\n[ERROR] HandTracker palm deinit failed\n");
        }
    }

    if (handlandmark_ctx != NULL) {
        if (mm_module_close(handlandmark_ctx) == NULL) {
            handlandmark_ctx = NULL;
        } else {
            printf("\r\n[ERROR] HandTracker landmark deinit failed\n");
        }
    }
}

// 每处理完一帧影像，NN 管线会呼叫一次（在 NN 的工作执行绪里）。
void HandTracker::onResult(void *p, void *img_param)
{
    if (!p || !img_param) {
        return;
    }

    vipnn_out_buf_t *out = (vipnn_out_buf_t *)p;
    handland_res_t *res = (handland_res_t *)&out->res[0];
    nn_data_param_t *im = (nn_data_param_t *)img_param;

    // 核心原本的程式在这里把影像尺寸改成 landmark 的 224x224，沿用。
    im->img.width = im->img.height = handlandmark_nn_roi.img.width;

    statCallbacks = statCallbacks + 1;
    if (res->handedness == HANDEDNESS_NOTFOUND) {
        statNotFound = statNotFound + 1;
        handFound = false;
        frameSeq = frameSeq + 1;
        return;
    }

    if (fixLandmark(res) < 0) {
        // 手在画面边缘、座标校正失败：当作这一帧没有手
        statFixFail = statFixFail + 1;
        handFound = false;
        frameSeq = frameSeq + 1;
        return;
    }

    const float size = (float)handlandmark_nn_roi.img.width;
    for (int i = 0; i < HAND_POINTS; i++) {
        float u = res->landmark3d.pos[i].x / size;
        float v = res->landmark3d.pos[i].y / size;
        LIMIT(u, 0.0f, 1.0f);
        LIMIT(v, 0.0f, 1.0f);
        landU[i] = u;
        landV[i] = v;
    }
    handFound = true;
    frameSeq = frameSeq + 1;
}

bool HandTracker::read(uint32_t &lastSeq, bool &found, float *u, float *v)
{
    const uint32_t seq = frameSeq;
    if (seq == lastSeq) {
        return false;
    }
    found = handFound;
    if (found) {
        // 回呼函式可能正在写入；多读一次比对序号，不一致就放弃这一帧。
        memcpy(u, landU, sizeof(landU));
        memcpy(v, landV, sizeof(landV));
        if (frameSeq != seq) {
            return false;
        }
    }
    lastSeq = seq;
    return true;
}

/*
  座标校正（照搬核心）：把 landmark 模型输出（经过旋转、缩放、平移的 224x224 空间）
  转回 NN 影像座标。
*/
int HandTracker::fixLandmark(handland_res_t *lm)
{
    float cx = (float)handlandmark_nn_roi.img.width / 2.0;
    float cy = (float)handlandmark_nn_roi.img.height / 2.0;
    float cos_theta = cosf(lm->theta);
    float sin_theta = sinf(lm->theta);
    float scale = lm->w / (float)(handlandmark_nn_roi.img.width / 2.0) / lm->ratio;
    for (int i = 0; i < HAND_LANDMARK_NUM; i++) {
        // 手靠在画面边缘时 landmark 有可能超出范围，先限制住
        LIMIT(lm->landmark3d.pos[i].x, 0, handlandmark_nn_roi.img.width);
        LIMIT(lm->landmark3d.pos[i].y, 0, handlandmark_nn_roi.img.height);
        float dx = lm->landmark3d.pos[i].x - cx;
        float dy = lm->landmark3d.pos[i].y - cy;
        float x = dx * cos_theta - dy * sin_theta + cx;
        float y = dx * sin_theta + dy * cos_theta + cy;
        if (x < 0 || y < 0) {
            return -1;
        }
        x = x * scale;
        y = y * scale;
        lm->landmark3d.pos[i].x = x + lm->offset_x;
        lm->landmark3d.pos[i].y = y + lm->offset_y;

        // 手在画面左上角时的额外修正（核心的做法）
        if (lm->offset_x < 20) {
            lm->landmark3d.pos[i].x = lm->landmark3d.pos[i].x - (20 - lm->offset_x);
            LIMIT(lm->landmark3d.pos[i].x, 0, handlandmark_nn_roi.img.width);
        }
        if (lm->offset_y < 20) {
            lm->landmark3d.pos[i].y = lm->landmark3d.pos[i].y - (20 - lm->offset_y);
            LIMIT(lm->landmark3d.pos[i].y, 0, handlandmark_nn_roi.img.height);
        }
    }
    return 0;
}
