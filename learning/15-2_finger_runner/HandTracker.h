#ifndef __HAND_TRACKER_H__
#define __HAND_TRACKER_H__

/*
  手部关键点追踪（掌心侦测 + 21 点手部关键点）

  为什么不直接用核心的 NNGestureDetection：
    这个 SDK 版本（AmebaPro2 4.0.15）的 NNGestureDetection 只会把手部骨架画在
    RTSP 影像的 OSD 上，getResultCount() 永远是 0、setResultCallback() 的回呼
    也从来不会被呼叫，xFeature()/yFeature() 甚至没有实作，所以程序拿不到任何
    关键点座标。

  这里把核心的 NNGestureDetection.cpp 复制过来改成 HandTracker：
    - 模型、管线、座标校正（fixLandmark）完全照搬，行为一致；
    - 原本「画到 OSD」的后处理改成「把 21 个关键点存起来」，不碰 OSD，所以不需要
      RTSP / FHD 的 channel 0。
  座标：存成 0~1 的比例，对应「NN 影像的方形区域」（landmark 模型的 224x224 空间）。
*/

#include "VideoStream.h"
#include "NNModelSelection.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "module_vipnn.h"
#ifdef __cplusplus
}
#endif

#define HAND_POINTS 21

class HandTracker : public NNModelSelection {
public:
    HandTracker(void);
    ~HandTracker(void);

    void configVideo(VideoSetting &config);
    void configThreshold(float confidence_threshold, float nms_threshold);
    void begin(void);
    void end(void);

    // 取出最新一笔结果。回传 true 代表有新的一帧（序号变了）。
    // found = 这一帧有没有看到手；u/v 各 21 个，0~1 的比例。
    static bool read(uint32_t &lastSeq, bool &found, float *u, float *v);

    // 诊断计数（序列埠每 5 秒印一次）：NN 回呼次数 / 没找到手 / 座标校正失败
    static volatile uint32_t statCallbacks;
    static volatile uint32_t statNotFound;
    static volatile uint32_t statFixFail;

private:
    static void onResult(void *p, void *img_param);
    static int fixLandmark(handland_res_t *lm);

    static nn_data_param_t palm_nn_roi;
    static nn_data_param_t handlandmark_nn_roi;

    static volatile uint32_t frameSeq;
    static volatile bool handFound;
    static float landU[HAND_POINTS];
    static float landV[HAND_POINTS];

    mm_context_t *handlandmark_ctx = NULL;
    void *palm_hand_siso_ctx = NULL;

    float pd_confidence_thresh = 0.5;
    float pd_nms_thresh = 0.3;
};

#endif
