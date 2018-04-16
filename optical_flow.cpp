#include <iostream>
#include <fstream>
#include <iomanip>
#include "opencv2/core.hpp"
#include <opencv2/core/utility.hpp>
#include "opencv2/highgui.hpp"
#include "opencv2/cudaoptflow.hpp"
#include "opencv2/cudaarithm.hpp"
#include "opencv2/opencv.hpp"
#include "opencv2/imgproc/imgproc.hpp"
using namespace std;
using namespace cv;
// using namespace cv::cuda;

inline bool isFlowCorrect(Point2f u)
{
    return !cvIsNaN(u.x) && !cvIsNaN(u.y) && fabs(u.x) < 1e9 && fabs(u.y) < 1e9;
}

static Vec3b computeColor(float fx, float fy)
{
    static bool first = true;

    // relative lengths of color transitions:
    // these are chosen based on perceptual similarity
    // (e.g. one can distinguish more shades between red and yellow
    //  than between yellow and green)
    const int RY = 15;
    const int YG = 6;
    const int GC = 4;
    const int CB = 11;
    const int BM = 13;
    const int MR = 6;
    const int NCOLS = RY + YG + GC + CB + BM + MR;
    static Vec3i colorWheel[NCOLS];

    if (first)
    {
        int k = 0;

        for (int i = 0; i < RY; ++i, ++k)
            colorWheel[k] = Vec3i(255, 255 * i / RY, 0);

        for (int i = 0; i < YG; ++i, ++k)
            colorWheel[k] = Vec3i(255 - 255 * i / YG, 255, 0);

        for (int i = 0; i < GC; ++i, ++k)
            colorWheel[k] = Vec3i(0, 255, 255 * i / GC);

        for (int i = 0; i < CB; ++i, ++k)
            colorWheel[k] = Vec3i(0, 255 - 255 * i / CB, 255);

        for (int i = 0; i < BM; ++i, ++k)
            colorWheel[k] = Vec3i(255 * i / BM, 0, 255);

        for (int i = 0; i < MR; ++i, ++k)
            colorWheel[k] = Vec3i(255, 0, 255 - 255 * i / MR);

        first = false;
    }

    const float rad = sqrt(fx * fx + fy * fy);
    const float a = atan2(-fy, -fx) / (float) CV_PI;

    const float fk = (a + 1.0f) / 2.0f * (NCOLS - 1);
    const int k0 = static_cast<int>(fk);
    const int k1 = (k0 + 1) % NCOLS;
    const float f = fk - k0;

    Vec4b pix;
    pix[3] = static_cast<uchar>(255.0);
    bool alpha = false;
    for (int b = 0; b < 3; b++)
    {
        const float col0 = colorWheel[k0][b] / 255.0f;
        const float col1 = colorWheel[k1][b] / 255.0f;

        float col = (1 - f) * col0 + f * col1;

        if (rad <= 1)
            col = 1 - rad * (1 - col); // increase saturation with radius
        else
            col *= .75; // out of range
        if (255.0 * col == 0.0) {
            alpha = true;
        }
        pix[2 - b] = static_cast<uchar>(255.0 * col);
    }
    if (alpha) {
        pix[3] = static_cast<uchar>(0.0);
    }
    return pix;
}

static void drawOpticalFlow(const Mat_<float>& flowx, const Mat_<float>& flowy, Mat& dst, float maxmotion = -1)
{
    dst.create(flowx.size(), CV_8UC4);  // change to CV_8UC4 to use the alpha channel.
    dst.setTo(Scalar::all(0));

    // determine motion range:
    float maxrad = maxmotion;

    if (maxmotion <= 0)
    {
        maxrad = 1;
        for (int y = 0; y < flowx.rows; ++y)
        {
            for (int x = 0; x < flowx.cols; ++x)
            {
                Point2f u(flowx(y, x), flowy(y, x));

                if (!isFlowCorrect(u))
                    continue;

                maxrad = max(maxrad, sqrt(u.x * u.x + u.y * u.y));
            }
        }
    }

    for (int y = 0; y < flowx.rows; ++y)
    {
        for (int x = 0; x < flowx.cols; ++x)
        {
            Point2f u(flowx(y, x), flowy(y, x));

            if (isFlowCorrect(u))
                dst.at<Vec4b>(y, x) = computeColor(u.x / maxrad, u.y / maxrad);
        }
    }
}


static void showFlow(const char* name, const cv::cuda::GpuMat& d_flow)
{
    cv::cuda::GpuMat planes[2];
    cuda::split(d_flow, planes);

    Mat flowx(planes[0]);
    Mat flowy(planes[1]);

    Mat out;
    drawOpticalFlow(flowx, flowy, out, 10);

    // imshow(name, out);
    imwrite(name, out);
}

int main(int argc, const char* argv[])
{
    string vpath;
    if (argc < 2) {
        cerr << "Usage : " << argv[0] << " <vpath>" << endl;
        return 0;
    } else {
        vpath = argv[1];
    }

    VideoCapture capture;
    Ptr<cuda::BroxOpticalFlow> brox = cuda::BroxOpticalFlow::create(0.197f, 50.0f, 0.8f, 10, 77, 10);
    capture.open(vpath);
    if (!capture.isOpened()) {
        cout << "Could not initialize capturing...\n" << endl;
        return -1;
    }
    int fcount = capture.get(CAP_PROP_FRAME_COUNT);
    Mat fPrev, prev;
    capture >> fPrev;
    if (fPrev.empty()) {
        cerr << "Fail to read the first frame" <<endl;
        return -1;
    }
    cvtColor(fPrev, prev, COLOR_BGR2GRAY);
    for (int i = 0; i < fcount - 1; i++) {
        Mat fCurr, curr;
        capture >> fCurr;
        if (fCurr.empty()) {
            cerr << "Can't open frame " + i << endl;
            return -1;
        }
        cvtColor(fCurr, curr, COLOR_BGR2GRAY);
        cv::cuda::GpuMat d_flow(prev.size(), CV_32FC2);
        cv::cuda::GpuMat d_frame0(prev);
        cv::cuda::GpuMat d_frame1(curr);
        cv::cuda::GpuMat d_frame0f;
        cv::cuda::GpuMat d_frame1f;

        d_frame0.convertTo(d_frame0f, CV_32F, 1.0 / 255.0);
        d_frame1.convertTo(d_frame1f, CV_32F, 1.0 / 255.0);

        const int64 start = getTickCount();

        brox->calc(d_frame0f, d_frame1f, d_flow);

        const double timeSec = (getTickCount() - start) / getTickFrequency();
        cout << "Brox in frame "  << i << " using " << timeSec << " sec " << endl;
        std::ostringstream stream;
        stream << std::setfill('0') << std::setw(5) << (i + 1) << ".jpg";

        showFlow(stream.str().c_str(), d_flow);
        prev = curr;
    }

    return 0;
}
