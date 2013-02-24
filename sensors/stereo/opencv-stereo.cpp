/**
 * Program that grabs stereo pairs from Point Grey
 * Firefly MV USB cameras, perfroms the single-disparity
 * stereo algorithm on them, and publishes the results
 * to LCM.
 *
 * Written by Andrew Barry <abarry@csail.mit.edu>, 2013
 *
 */
 
#include <cv.h>
#include <highgui.h>
#include "opencv2/legacy/legacy.hpp"
#include <sys/time.h>

#include "opencv2/opencv.hpp"

#include <GL/gl.h>
#include <lcm/lcm.h>
#include <bot_lcmgl_client/lcmgl.h>

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

//#include <libusb.h> // for USB reset

#include "barrymoore.hpp"

using namespace std;
using namespace cv;

extern "C"
{
    #include <stdio.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <inttypes.h>

    #include <dc1394/dc1394.h>

    #include "camera.h"
    #include "utils.h"
}

//#define POINT_GREY_VENDOR_ID 0x1E10
//#define POINT_GREY_PRODUCT_ID 0x2000

#define BRIGHTNESS_VALUE 78
#define EXPOSURE_VALUE 128

//#define USE_IMAGE // uncomment to use left.jpg and right.jpg as test images

struct RemapState
{
    Mat inputImage;
    Mat outputImage;
    Mat map1;
    Mat map2;
    int flags;
};

void PublishToLcm(bot_lcmgl_t* lcmgl, Mat imgDisp, Mat Q, Mat leftImg);
Mat GetFrameFormat7(dc1394camera_t *camera);

bool ResetPointGreyCameras();


dc1394_t        *d;
dc1394camera_t  *camera;

dc1394_t        *d2;
dc1394camera_t  *camera2;

/**
 * Cleanly handles and exit from a command-line
 * ctrl-c signal.
 *
 * @param s
 *
 */
void control_c_handler(int s)
{
    cout << endl << "exiting via ctrl-c" << endl;

    dc1394_video_set_transmission(camera, DC1394_OFF);
    dc1394_capture_stop(camera);
    dc1394_camera_free(camera);
    
    dc1394_video_set_transmission(camera2, DC1394_OFF);
    dc1394_capture_stop(camera2);
    dc1394_camera_free(camera2);
    
    dc1394_free (d);
    dc1394_free (d2);
    
    exit(1);

}

int main(int argc, char *argv[])
{
    
    dc1394error_t   err;
    uint64         guid = 0x00b09d0100af04d8; // camera 1
    
    unsigned long elapsed;
    int numFrames = 0;
    
    // ----- cam 2 -----
    
    dc1394error_t   err2;
    uint64         guid2 = 0x00b09d0100a01ac5; // camera2
    
    // setup control-c handling
    struct sigaction sigIntHandler;

    sigIntHandler.sa_handler = control_c_handler;
    sigemptyset(&sigIntHandler.sa_mask);
    sigIntHandler.sa_flags = 0;

    sigaction(SIGINT, &sigIntHandler, NULL);
    // end ctrl-c handling code
    
    // tell opencv to use only one core so that we can manage our
    // own threading without a fight
    setNumThreads(1);
    
    #ifndef USE_IMAGE
        d = dc1394_new ();
        if (!d)
            cerr << "Could not create dc1394 context" << endl;
            
        d2 = dc1394_new ();
        if (!d2)
            cerr << "Could not create dc1394 context for camera 2" << endl;

        camera = dc1394_camera_new (d, guid);
        if (!camera)
        {
            cerr << "Could not create dc1394 camera... attempting bus reset" << endl;
            #if 0
            if (!ResetPointGreyCameras())
            {
                // failed
                cerr << "Failed to reset USB cameras.  Quitting." << endl;
                exit(1);
            }
            #endif
        }
        
        camera2 = dc1394_camera_new (d2, guid2);
        if (!camera2)
            cerr << "Could not create dc1394 camera for camera 2" << endl;
        // reset the bus
        dc1394_reset_bus(camera);
        dc1394_reset_bus(camera2);
        
        // setup
        err = setup_gray_capture(camera, DC1394_VIDEO_MODE_FORMAT7_1);
        DC1394_ERR_CLN_RTN(err, cleanup_and_exit(camera), "Could not setup camera");

        err2 = setup_gray_capture(camera2, DC1394_VIDEO_MODE_FORMAT7_1);
        DC1394_ERR_CLN_RTN(err2, cleanup_and_exit(camera2), "Could not setup camera number 2");
        
        // enable auto-exposure
        // turn on the auto exposure feature
        err = dc1394_feature_set_power(camera, DC1394_FEATURE_EXPOSURE, DC1394_ON);
        DC1394_ERR_RTN(err,"Could not turn on the exposure feature");
        
        err = dc1394_feature_set_mode(camera, DC1394_FEATURE_EXPOSURE, DC1394_FEATURE_MODE_ONE_PUSH_AUTO);
        DC1394_ERR_RTN(err,"Could not turn on Auto-exposure");
        
        // enable auto-exposure
        // turn on the auto exposure feature
        err = dc1394_feature_set_power(camera2, DC1394_FEATURE_EXPOSURE, DC1394_ON);
        DC1394_ERR_RTN(err,"Could not turn on the exposure feature for cam2");
        
        err = dc1394_feature_set_mode(camera2, DC1394_FEATURE_EXPOSURE, DC1394_FEATURE_MODE_ONE_PUSH_AUTO);
        DC1394_ERR_RTN(err,"Could not turn on Auto-exposure for cam2");
        
        // enable camera
        err = dc1394_video_set_transmission(camera, DC1394_ON);
        DC1394_ERR_CLN_RTN(err, cleanup_and_exit(camera), "Could not start camera iso transmission");
        err2 = dc1394_video_set_transmission(camera2, DC1394_ON);
        DC1394_ERR_CLN_RTN(err2, cleanup_and_exit(camera2), "Could not start camera iso transmission for camera number 2");
    #endif

    namedWindow("Input", CV_WINDOW_AUTOSIZE);
    namedWindow("Input2", CV_WINDOW_AUTOSIZE);
    namedWindow("Stereo", CV_WINDOW_AUTOSIZE);
    namedWindow("Depth", CV_WINDOW_AUTOSIZE);
    
    
    // load calibration

    Mat qMat, mx1Mat, my1Mat, mx2Mat, my2Mat;

    CvMat *Q = (CvMat *)cvLoad("calib/Q.xml",NULL,NULL,NULL);
    CvMat *mx1 = (CvMat *)cvLoad("calib/mx1.xml",NULL,NULL,NULL);
    CvMat *my1 = (CvMat *)cvLoad("calib/my1.xml",NULL,NULL,NULL);
    CvMat *mx2 = (CvMat *)cvLoad("calib/mx2.xml",NULL,NULL,NULL);
    CvMat *my2 = (CvMat *)cvLoad("calib/my2.xml",NULL,NULL,NULL);
    
    qMat = Mat(Q, true);
    mx1Mat = Mat(mx1,true);
    my1Mat = Mat(my1,true);
    mx2Mat = Mat(mx2,true);
    my2Mat = Mat(my2,true);
    
    Mat mx1fp, empty1, mx2fp, empty2;
    
    // this will convert to a fixed-point notation
    convertMaps(mx1Mat, my1Mat, mx1fp, empty1, CV_16SC2, true);
    convertMaps(mx2Mat, my2Mat, mx2fp, empty2, CV_16SC2, true);

    // start up LCM
    lcm_t * lcm;
    lcm = lcm_create("udpm://239.255.76.67:7667?ttl=1");
    bot_lcmgl_t* lcmgl = bot_lcmgl_init(lcm, "lcmgl-stereo");
    
    Mat imgDisp;
    Mat imgDisp2;
    
    // initilize default parameters
    BarryMooreState state;
    
    state.disparity = 30;
    state.sobelLimit = 260;
    state.blockSize = 5;
    state.sadThreshold = 79; //50
    
    state.mapxL = mx1fp;
    state.mapxR = mx2fp;
    state.Q = qMat;
    
    Mat depthMap;
    
    bool quit = false;
    
    Mat matL, matR;
    
    #ifdef USE_IMAGE
        matL = imread("left.jpg", -1);
        matR = imread("right.jpg", -1);
    #endif


    // start the framerate clock
    struct timeval start, now;
    gettimeofday( &start, NULL );
    
    while (quit == false) {
    
        // get the frames from the camera
        #ifndef USE_IMAGE
            matL = GetFrameFormat7(camera);
            matR = GetFrameFormat7(camera2);
        #endif
        
        Mat matDisp;
        
        #if SHOW_DISPLAY
        // we remap again here because we're just in display
        Mat remapL(matL.rows, matL.cols, matL.depth());
        Mat remapR(matR.rows, matR.cols, matR.depth());
        
        if (numFrames == 0)
        {
            depthMap = Mat::zeros(matL.rows, matL.cols, CV_8UC1);
        }
        
        remap(matL, remapL, mx1fp, Mat(), INTER_NEAREST);
        remap(matR, remapR, mx2fp, Mat(), INTER_NEAREST);
        
        remapL.copyTo(matDisp);
        #endif
        
        cv::vector<Point3f> pointVector3d;
        cv::vector<Point> pointVector2d; // for display
        
        
        StereoBarryMoore(matL, matR, &pointVector3d, &pointVector2d, state);
        
        
        // publish points to the 3d map
        //PublishToLcm(lcmgl, matDisp, Q, matR);
        
        // prep disparity image for display
        //normalize(matDisp, matDisp, 0, 256, NORM_MINMAX, CV_8S);
        
        // display images
        
        bot_lcmgl_push_matrix(lcmgl);
        bot_lcmgl_point_size(lcmgl, 10.5f);
        bot_lcmgl_begin(lcmgl, GL_POINTS);
        
        for (unsigned int i=0;i<pointVector3d.size();i++)
        {
            float x = pointVector3d[i].x;
            float y = pointVector3d[i].y;
            float z = pointVector3d[i].z;
            // publish to LCM
            
            #if SHOW_DISPLAY
             #if 0
             //cout << "(" << x << ", " << y << ", " << z << ")" << endl;
            
            // get the color of the pixel
            int x2 = pointVector2d[i].x;
            int y2 = pointVector2d[i].y;
            double factor = 2.0;
            //int delta = (state.blockSize-1)/2;
            int delta = 20;
           
            for (int row=y2-delta;row<y2+delta;row++)
            {
                int rowdelta = row-y2*factor;
                for (int col = x2-delta; col < x2+delta; col++)
                {
                    int coldelta = col-x2*factor;
                
                    uchar pxL = remapL.at<uchar>(row, col);
                    float gray = pxL/255.0;
                
                    bot_lcmgl_color3f(lcmgl, gray, gray, gray);                    
                    bot_lcmgl_vertex3f(lcmgl, x+coldelta, -y-rowdelta, z);
                }
            }
            #endif
            
            #endif
            
            bot_lcmgl_vertex3f(lcmgl, z, x, -y);
        }
        
        bot_lcmgl_end(lcmgl);
        bot_lcmgl_pop_matrix(lcmgl);

        //if (numFrames%200 == 0)
        {
        bot_lcmgl_switch_buffer(lcmgl);
        }
        #if SHOW_DISPLAY
        
        for (unsigned int i=0;i<pointVector2d.size();i++)
        {
            int x2 = pointVector2d[i].x;
            int y2 = pointVector2d[i].y;
            rectangle(matDisp, Point(x2,y2), Point(x2+state.blockSize, y2+state.blockSize), 0);
            rectangle(matDisp, Point(x2+1,y2+1), Point(x2+state.blockSize-1, y2-1+state.blockSize), 255);
            
            int gray = 337.0 - state.disparity*41.0/6;
            
            // fill in the depth map at this point
            circle(depthMap, Point(x2,y2), 5, gray, -1);
        }
        
        imshow("Input", remapL);
        imshow("Input2", remapR);
        imshow("Stereo", matDisp);
        imshow("Depth", depthMap);
            
        char key = waitKey(1);
        
        if (key != 255)
        {
            cout << endl << key << endl;
        }
        
        switch (key)
        {
            case 'T':
                state.disparity --;
                break;
            case 'R':
                state.disparity ++;
                break;
                
            case 'w':
                state.sobelLimit += 10;
                break;
                
            case 's':
                state.sobelLimit -= 10;
                break;
                
            case 'g':
                state.blockSize ++;
                break;
                
            case 'b':
                state.blockSize --;
                break;
                
            case 'y':
                state.sadThreshold ++;
                break;
                
            case 'h':
                state.sadThreshold --;
                break;
                
            case 'q':
                quit = true;
                break;
        }
        
        if (key != 255)
        {
            cout << "disparity = " << state.disparity << endl;
            cout << "sobelLimit = " << state.sobelLimit << endl;
            cout << "blockSize = " << state.blockSize << endl;
            cout << "sadThreshold = " << state.sadThreshold << endl;
        }
        #endif
        
        numFrames ++;    
            
        // compute framerate
        gettimeofday( &now, NULL );
        
        elapsed = (now.tv_usec / 1000 + now.tv_sec * 1000) - 
        (start.tv_usec / 1000 + start.tv_sec * 1000);
        
        printf("\r%d frames (%lu ms) - %4.1f fps | %4.1f ms/frame", numFrames, elapsed, (float)numFrames/elapsed * 1000, elapsed/(float)numFrames);
        fflush(stdout);

        
    }

    printf("\n\n");
    
    destroyWindow("Input");
    destroyWindow("Input2");
    destroyWindow("Stereo");
    
    // stop data transmission
    err = dc1394_video_set_transmission(camera, DC1394_OFF);
    DC1394_ERR_CLN_RTN(err,cleanup_and_exit(camera),"Could not stop the camera");
    
    err2 = dc1394_video_set_transmission(camera2, DC1394_OFF);
    DC1394_ERR_CLN_RTN(err2,cleanup_and_exit(camera2),"Could not stop the camera 2");

    // close camera
    dc1394_video_set_transmission(camera, DC1394_OFF);
    dc1394_capture_stop(camera);
    dc1394_camera_free(camera);
    
    dc1394_video_set_transmission(camera2, DC1394_OFF);
    dc1394_capture_stop(camera2);
    dc1394_camera_free(camera2);
    
    dc1394_free (d);
    dc1394_free (d2);
    
    return 0;
}

void PublishToLcm(bot_lcmgl_t* lcmgl, Mat imgDisp, Mat Q, Mat leftImg)
{

    // ok so now, parse out the points
    Mat reprojected;

    reprojectImageTo3D(imgDisp, reprojected, Q, true);

    // now create points in lcmgl using the reprojected image


    bot_lcmgl_push_matrix(lcmgl);
    bot_lcmgl_point_size(lcmgl, 10.5f);
    bot_lcmgl_begin(lcmgl, GL_POINTS);  // render as points
    
    float *p;
    uchar *imgP;

    for (int i=0; i < imgDisp.rows; i++)
    {
        p = reprojected.ptr<float>(i);
        
        imgP = leftImg.ptr<uchar>(i);
        
        for (int j=0; j < imgDisp.cols; j++)
        {
            // for each point, get the xyz value and make an opengl object for it
            // set the color of the point
            
            float gray = imgP[j]/255.0;
            
            //cout << imgP[j] << "...." << gray << endl;cout << imgP[j/3] << gray << endl;
            
            if (p[3*j+2] < 9000) // filter outliers
            {
                bot_lcmgl_color3f(lcmgl, gray, gray, gray);
                //bot_lcmgl_vertex3f(lcmgl, i,j,0);
                bot_lcmgl_vertex3f(lcmgl, -p[3*j],  p[3*j+1], -p[3*j+2]);
            }
        }
    }



    bot_lcmgl_end(lcmgl);
    bot_lcmgl_pop_matrix(lcmgl);

    bot_lcmgl_switch_buffer(lcmgl);
  
}

/**
 * Gets a Format7 frame from a Firefly MV USB camera.
 * The frame will be CV_8UC1 and black and white.
 *
 * @param camera the camrea
 *
 * @retval frame as an OpenCV Mat
 */
Mat GetFrameFormat7(dc1394camera_t *camera)
{
    // get a frame
    dc1394error_t err;
    dc1394video_frame_t *frame;

    err = dc1394_capture_dequeue(camera, DC1394_CAPTURE_POLICY_WAIT, &frame);
    DC1394_WRN(err,"Could not capture a frame");
    
    // make a Mat of the right size and type that we attach the the existing data
    Mat matTmp = Mat(frame->size[1], frame->size[0], CV_8UC1, frame->image, frame->size[0]);
    
    // copy the data out of the ringbuffer so we can give the buffer back
    Mat matOut = matTmp.clone();
    
    // release the buffer
    err = dc1394_capture_enqueue(camera, frame);
    DC1394_WRN(err,"releasing buffer");
    
    return matOut;
}

# if 0
/**
 * Function that finds and resets all connected USB Point Grey cameras.
 *
 * @retval true on successful reset, false on failure.
 */
bool ResetPointGreyCameras()
{
    libusb_context *s_libucontext;
    
    // open the libusb context
    libusb_init(&s_libucontext);
    
    libusb_device **list;

    ssize_t ret;

    ret = libusb_get_device_list (s_libucontext, &list);

    if (ret == LIBUSB_ERROR_NO_MEM || ret < 1)
    {
        printf("Failed to find device in the libusb context.\n");
        libusb_free_device_list(list, 1);
        libusb_exit(s_libucontext);
        return false;
    }
    
    int j;
    int libusb_flag = 0;
    
    for (j=0; j<ret; j++)
    {
        libusb_device *this_device = list[j];
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(this_device, &desc) == 0)
        {
            // before attempting to open the device and get it's description, look at the vendor and product IDs
            // this also means that we won't attempt to open a bunch of devices we probably don't have permissions to open
            // furthermore, if the user hasn't set up permissions correctly, libusb will print out an error about the permissions
            // giving a good hint at what to do
            
            cout << desc.idVendor << endl;
            
            if (desc.idVendor == POINT_GREY_VENDOR_ID && desc.idProduct == POINT_GREY_PRODUCT_ID)
            {
                cout << "found a hit" << endl;

                libusb_device_handle *udev;
                if (libusb_open(this_device, &udev) == 0)
                {
                    cout << "resetting device." << endl;
                    // reset this device
                    cout << libusb_reset_device(udev);
                    cout << "success." << endl;
                    //libusb_close(udev);
                    libusb_flag = 1;
                } else
                {
                    cout << "failed to open device" << endl;
                }
            }
        }
    }
    
    // cleanup libusb list
    libusb_free_device_list(list, 1);
    
    libusb_exit(s_libucontext);

    if (libusb_flag == 0)
    {
        printf("Failed to reset the USB cameras.\n");
        return false;
    }
    
    return true;
    
}
#endif