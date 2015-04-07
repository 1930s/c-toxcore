#include "../toxav/toxav.h"
#include "../toxcore/tox.h"

/* For playing audio data */
#include <AL/al.h>
#include <AL/alc.h>

/* Processing wav's */
#include <sndfile.h>

/* For reading and displaying video data */
#include <opencv/cv.h>
#include <opencv/highgui.h>
#include <opencv/cvwimage.h>


#include <sys/stat.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>


#define c_sleep(x) usleep(1000*x)


#define CLIP(X) ( (X) > 255 ? 255 : (X) < 0 ? 0 : X)

// RGB -> YUV
#define RGB2Y(R, G, B) CLIP(( (  66 * (R) + 129 * (G) +  25 * (B) + 128) >> 8) +  16)
#define RGB2U(R, G, B) CLIP(( ( -38 * (R) -  74 * (G) + 112 * (B) + 128) >> 8) + 128)
#define RGB2V(R, G, B) CLIP(( ( 112 * (R) -  94 * (G) -  18 * (B) + 128) >> 8) + 128)

// YUV -> RGB
#define C(Y) ( (Y) - 16  )
#define D(U) ( (U) - 128 )
#define E(V) ( (V) - 128 )

#define YUV2R(Y, U, V) CLIP(( 298 * C(Y)              + 409 * E(V) + 128) >> 8)
#define YUV2G(Y, U, V) CLIP(( 298 * C(Y) - 100 * D(U) - 208 * E(V) + 128) >> 8)
#define YUV2B(Y, U, V) CLIP(( 298 * C(Y) + 516 * D(U)              + 128) >> 8)


/* Enable/disable tests */
#define TEST_REGULAR_AV 0
#define TEST_REGULAR_A 0
#define TEST_REGULAR_V 0
#define TEST_REJECT 0
#define TEST_CANCEL 0
#define TEST_MUTE_UNMUTE 0
#define TEST_TRANSFER_A 1
#define TEST_TRANSFER_V 0


typedef struct {
    bool incoming;
    uint32_t state;
} CallControl;

struct toxav_thread_data {
    ToxAV*  AliceAV;
    ToxAV*  BobAV;
    int32_t sig;
};

const char* vdout = "AV Test";
uint32_t adout;

const char* stringify_state(TOXAV_CALL_STATE s)
{
    static const char* strings[] =
    {
        "NOT SENDING",
        "SENDING AUDIO",
        "SENDING VIDEO",
        "SENDING AUDIO AND VIDEO",
        "PAUSED",
        "END",
        "ERROR"
    };
    
    return strings [s];
}

/** 
 * Callbacks 
 */
void t_toxav_call_cb(ToxAV *av, uint32_t friend_number, bool audio_enabled, bool video_enabled, void *user_data)
{
    printf("Handling CALL callback\n");
    ((CallControl*)user_data)->incoming = true;
}
void t_toxav_call_state_cb(ToxAV *av, uint32_t friend_number, uint32_t state, void *user_data)
{
    printf("Handling CALL STATE callback: %d\n", state);
    
    ((CallControl*)user_data)->state = state;
}
void t_toxav_receive_video_frame_cb(ToxAV *av, uint32_t friend_number,
                                    uint16_t width, uint16_t height,
                                    uint8_t const *planes[], int32_t const stride[],
                                    void *user_data)
{
    uint16_t *img_data = malloc(height * width * 6);
    
    unsigned long int i, j;
    for (i = 0; i < height; ++i) {
        for (j = 0; j < width; ++j) {
            uint8_t *point = (void*)img_data + 3 * ((i * width) + j);
            int y = planes[0][(i * stride[0]) + j];
            int u = planes[1][((i / 2) * stride[1]) + (j / 2)];
            int v = planes[2][((i / 2) * stride[2]) + (j / 2)];
            
            point[0] = YUV2R(y, u, v);
            point[1] = YUV2G(y, u, v);
            point[2] = YUV2B(y, u, v);
        }
    }
    
    
    CvMat mat = cvMat(height, width, CV_8UC3, img_data);
    
    CvSize sz = {.height = height, .width = width};
    
    IplImage* header = cvCreateImageHeader(sz, 1, 3);
    IplImage* img = cvGetImage(&mat, header);
    cvShowImage(vdout, img);
    free(img_data);
}
void t_toxav_receive_audio_frame_cb(ToxAV *av, uint32_t friend_number,
                                    int16_t const *pcm,
                                    size_t sample_count,
                                    uint8_t channels,
                                    uint32_t sampling_rate,
                                    void *user_data)
{
    uint32_t bufid;
    int32_t processed = 0, queued = 16;
    alGetSourcei(adout, AL_BUFFERS_PROCESSED, &processed);
    alGetSourcei(adout, AL_BUFFERS_QUEUED, &queued);

    if(processed) {
        uint32_t bufids[processed];
        alSourceUnqueueBuffers(adout, processed, bufids);
        alDeleteBuffers(processed - 1, bufids + 1);
//         bufid = bufids[0];
    }
//     else if(queued < 16)
        alGenBuffers(1, &bufid);
//     else
//         return;
    

    alBufferData(bufid, channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16,
                 pcm, sample_count * 2, sampling_rate);
    alSourceQueueBuffers(adout, 1, &bufid);

    int32_t state;
    alGetSourcei(adout, AL_SOURCE_STATE, &state);

    if(state != AL_PLAYING) 
        alSourcePlay(adout);
}
void t_accept_friend_request_cb(Tox *m, const uint8_t *public_key, const uint8_t *data, uint16_t length, void *userdata)
{
    if (length == 7 && memcmp("gentoo", data, 7) == 0) {
        tox_add_friend_norequest(m, public_key);
    }
}


/**
 */
void initialize_tox(Tox** bootstrap, ToxAV** AliceAV, CallControl* AliceCC, ToxAV** BobAV, CallControl* BobCC)
{
    Tox* Alice;
    Tox* Bob;
    
    *bootstrap = tox_new(0);
    Alice = tox_new(0);
    Bob = tox_new(0);
    
    assert(bootstrap && Alice && Bob);
    
    printf("Created 3 instances of Tox\n");
    
    printf("Preparing network...\n");
    long long unsigned int cur_time = time(NULL);
    
    uint32_t to_compare = 974536;
    uint8_t address[TOX_FRIEND_ADDRESS_SIZE];
    
    tox_callback_friend_request(Alice, t_accept_friend_request_cb, &to_compare);
    tox_get_address(Alice, address);
    
    assert(tox_add_friend(Bob, address, (uint8_t *)"gentoo", 7) >= 0);
    
    uint8_t off = 1;
    
    while (1) {
        tox_do(*bootstrap);
        tox_do(Alice);
        tox_do(Bob);
        
        if (tox_isconnected(*bootstrap) && tox_isconnected(Alice) && tox_isconnected(Bob) && off) {
            printf("Toxes are online, took %llu seconds\n", time(NULL) - cur_time);
            off = 0;
        }
        
        if (tox_get_friend_connection_status(Alice, 0) == 1 && tox_get_friend_connection_status(Bob, 0) == 1)
            break;
        
        c_sleep(20);
    }
    
    
    TOXAV_ERR_NEW rc;
    *AliceAV = toxav_new(Alice, &rc);
    assert(rc == TOXAV_ERR_NEW_OK);
    
    *BobAV = toxav_new(Bob, &rc);
    assert(rc == TOXAV_ERR_NEW_OK);
    
    /* Alice */
    toxav_callback_call(*AliceAV, t_toxav_call_cb, AliceCC);
    toxav_callback_call_state(*AliceAV, t_toxav_call_state_cb, AliceCC);
    toxav_callback_receive_video_frame(*AliceAV, t_toxav_receive_video_frame_cb, AliceCC);
    toxav_callback_receive_audio_frame(*AliceAV, t_toxav_receive_audio_frame_cb, AliceCC);
    
    /* Bob */
    toxav_callback_call(*BobAV, t_toxav_call_cb, BobCC);
    toxav_callback_call_state(*BobAV, t_toxav_call_state_cb, BobCC);
    toxav_callback_receive_video_frame(*BobAV, t_toxav_receive_video_frame_cb, BobCC);
    toxav_callback_receive_audio_frame(*BobAV, t_toxav_receive_audio_frame_cb, BobCC);
    
    printf("Created 2 instances of ToxAV\n");
    printf("All set after %llu seconds!\n", time(NULL) - cur_time);
}
int iterate_tox(Tox* bootstrap, ToxAV* AliceAV, ToxAV* BobAV)
{
    tox_do(bootstrap);
    tox_do(toxav_get_tox(AliceAV));
    tox_do(toxav_get_tox(BobAV));
    
    return MIN(tox_do_interval(toxav_get_tox(AliceAV)), tox_do_interval(toxav_get_tox(BobAV)));
}
void* iterate_toxav (void * data)
{   
    struct toxav_thread_data* data_cast = data;
    
//     cvNamedWindow(vdout, CV_WINDOW_AUTOSIZE);
    
    while (data_cast->sig == 0) {
        toxav_iterate(data_cast->AliceAV);
        toxav_iterate(data_cast->BobAV);
        int rc = MIN(toxav_iteration_interval(data_cast->AliceAV), toxav_iteration_interval(data_cast->BobAV));
        
        printf("\rToxAV interval: %d   ", rc);
        fflush(stdout);
        cvWaitKey(rc);
    }
    
    data_cast->sig = 1;
    
//     cvDestroyWindow(vdout);
    pthread_exit(NULL);
}

int send_opencv_img(ToxAV* av, uint32_t friend_number, const IplImage* img)
{   
    int32_t strides[3] = { 1280, 640, 640 };
    uint8_t* planes[3] = {
        malloc(img->height * img->width),
        malloc(img->height * img->width / 2),
        malloc(img->height * img->width / 2),
    };
    
    int x_chroma_shift = 1;
    int y_chroma_shift = 1;
    
    int x, y;
    for (y = 0; y < img->height; ++y) {
        for (x = 0; x < img->width; ++x) {
            uint8_t r = img->imageData[(x + y * img->width) * 3 + 0];
            uint8_t g = img->imageData[(x + y * img->width) * 3 + 1];
            uint8_t b = img->imageData[(x + y * img->width) * 3 + 2];
            
            planes[0][x + y * strides[0]] = RGB2Y(r, g, b);
            if (!(x % (1 << x_chroma_shift)) && !(y % (1 << y_chroma_shift))) {
                const int i = x / (1 << x_chroma_shift);
                const int j = y / (1 << y_chroma_shift);
                planes[1][i + j * strides[1]] = RGB2U(r, g, b);
                planes[2][i + j * strides[2]] = RGB2V(r, g, b);
            }
        }
    }
    
    
    int rc = toxav_send_video_frame(av, friend_number, img->width, img->height, planes[0], planes[1], planes[2], NULL);
    free(planes[0]);
    free(planes[1]);
    free(planes[2]);
    return rc;
}

ALCdevice* open_audio_device(const char* audio_out_dev_name)
{
    ALCdevice* rc;
    rc = alcOpenDevice(audio_out_dev_name);
    if ( !rc ) {
        printf("Failed to open playback device: %s: %d\n", audio_out_dev_name, alGetError());
        exit(1);
    }
    
    ALCcontext* out_ctx = alcCreateContext(rc, NULL);
    alcMakeContextCurrent(out_ctx);
    
    alGenSources((uint32_t)1, &adout);
    alSourcei(adout, AL_LOOPING, AL_FALSE);
    alSourcePlay(adout);
    
    return rc;
}

int print_audio_devices()
{
    const char *device;

    printf("Default output device: %s\n", alcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER));
    printf("Output devices:\n");
    
    int i = 0;
    for(device = alcGetString(NULL, ALC_DEVICE_SPECIFIER); *device; 
        device += strlen( device ) + 1, ++i) {
        printf("%d) %s\n", i, device);
    }
    
    return 0;
}

int print_help (const char* name)
{
    printf("Usage: %s -[a:v:o:dh]\n"
           "-a <path> audio input file\n"
           "-b <ms> audio frame duration\n"
           "-v <path> video input file\n"
           "-x <ms> video frame duration\n"
           "-o <idx> output audio device index\n"
           "-d print output audio devices\n"
           "-h print this help\n", name);
    
    return 0;
}


int main (int argc, char** argv)
{
    struct stat st;
    
    /* AV files for testing */
    const char* af_name = NULL;
    const char* vf_name = NULL;
    long audio_out_dev_idx = 0;
    
    int32_t audio_frame_duration = 20;
    int32_t video_frame_duration = 10;
    
    /* Parse settings */
    CHECK_ARG: switch (getopt(argc, argv, "a:b:v:x:o:dh")) {
    case 'a':
        af_name = optarg;
        goto CHECK_ARG;
    case 'b':{
        char *d;
        audio_frame_duration = strtol(optarg, &d, 10);
        if (*d) {
            printf("Invalid value for argument: 'b'");
            exit(1);
        }
        goto CHECK_ARG;
    }
    case 'v':
        vf_name = optarg;
        goto CHECK_ARG;
    case 'x':{
        char *d;
        video_frame_duration = strtol(optarg, &d, 10);
        if (*d) {
            printf("Invalid value for argument: 'x'");
            exit(1);
        }
        goto CHECK_ARG;
    }
    case 'o': {
        char *d;
        audio_out_dev_idx = strtol(optarg, &d, 10);
        if (*d) {
            printf("Invalid value for argument: 'o'");
            exit(1);
        }
        goto CHECK_ARG;
    }
    case 'd':
        return print_audio_devices();
    case 'h':
        return print_help(argv[0]);
    case '?':
        exit(1);
    case -1:;
    }
    
    { /* Check files */
        if (!af_name) {
            printf("Required audio input file!\n");
            exit(1);
        }
        
        if (!vf_name) {
            printf("Required video input file!\n");
            exit(1);
        }
        
        /* Check for files */
        if(stat(af_name, &st) != 0 || !S_ISREG(st.st_mode))
        {
            printf("%s doesn't seem to be a regular file!\n", af_name);
            exit(1);
        }
        
        if(stat(vf_name, &st) != 0 || !S_ISREG(st.st_mode))
        {
            printf("%s doesn't seem to be a regular file!\n", vf_name);
            exit(1);
        }
    }
    
    const char* audio_out_dev_name = NULL;
    
    int i = 0;
    for(audio_out_dev_name = alcGetString(NULL, ALC_DEVICE_SPECIFIER); i < audio_out_dev_idx;
        audio_out_dev_name += strlen( audio_out_dev_name ) + 1, ++i)
        if (!(audio_out_dev_name + strlen( audio_out_dev_name ) + 1))
            break;
	
    printf("Using audio device: %s\n", audio_out_dev_name);
    printf("Using audio file: %s\n", af_name);
    printf("Using video file: %s\n", vf_name);
    
    if (0) {
        /* Open audio file */
        SF_INFO af_info;
        SNDFILE* af_handle = sf_open(af_name, SFM_READ, &af_info);
        if (af_handle == NULL)
        {
            printf("Failed to open the file.\n");
            exit(1);
        }
        ALCdevice* audio_out_device = open_audio_device(audio_out_dev_name);
        
        
        int16_t PCM[5760];
        
        time_t start_time = time(NULL);
        time_t expected_time = af_info.frames / af_info.samplerate + 2;
        
        printf("Sample rate %d\n", af_info.samplerate);
        while ( start_time + expected_time > time(NULL) ) {
            int frame_size = (af_info.samplerate * audio_frame_duration / 1000) * af_info.channels;
            
            int64_t count = sf_read_short(af_handle, PCM, frame_size);
            if (count > 0)
                t_toxav_receive_audio_frame_cb(NULL, 0, PCM, count, af_info.channels, af_info.samplerate, NULL);
            c_sleep(audio_frame_duration);
        }
    
        
        printf("Played file in: %lu\n", time(NULL) - start_time);
        
        alcCloseDevice(audio_out_device);
        sf_close(af_handle);
        return 0;
    }
    /* START TOX NETWORK */
    
    Tox *bootstrap;
    ToxAV *AliceAV;
    ToxAV *BobAV;
    
    CallControl AliceCC;
    CallControl BobCC;
    
    initialize_tox(&bootstrap, &AliceAV, &AliceCC, &BobAV, &BobCC);
    
#define REGULAR_CALL_FLOW(A_BR, V_BR) \
	do { \
        memset(&AliceCC, 0, sizeof(CallControl)); \
        memset(&BobCC, 0, sizeof(CallControl)); \
        \
        TOXAV_ERR_CALL rc; \
        toxav_call(AliceAV, 0, A_BR, V_BR, &rc); \
        \
        if (rc != TOXAV_ERR_CALL_OK) { \
            printf("toxav_call failed: %d\n", rc); \
            exit(1); \
        } \
        \
        \
        long long unsigned int start_time = time(NULL); \
        \
        \
        while (BobCC.state != TOXAV_CALL_STATE_END) { \
            \
            if (BobCC.incoming) { \
                TOXAV_ERR_ANSWER rc; \
                toxav_answer(BobAV, 0, A_BR, V_BR, &rc); \
                \
                if (rc != TOXAV_ERR_ANSWER_OK) { \
                    printf("toxav_answer failed: %d\n", rc); \
                    exit(1); \
                } \
                BobCC.incoming = false; \
            } else { \
                /* TODO rtp */ \
                \
                if (time(NULL) - start_time == 5) { \
                    \
                    TOXAV_ERR_CALL_CONTROL rc; \
                    toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_CANCEL, &rc); \
                    \
                    if (rc != TOXAV_ERR_CALL_CONTROL_OK) { \
                        printf("toxav_call_control failed: %d\n", rc); \
                        exit(1); \
                    } \
                } \
            } \
             \
            iterate(bootstrap, AliceAV, BobAV); \
        } \
        printf("Success!\n");\
    } while(0)
    
    if (TEST_REGULAR_AV) {
		printf("\nTrying regular call (Audio and Video)...\n");
		REGULAR_CALL_FLOW(48, 4000);
	}
	
    if (TEST_REGULAR_A) {
		printf("\nTrying regular call (Audio only)...\n");
		REGULAR_CALL_FLOW(48, 0);
	}
	
	if (TEST_REGULAR_V) {
		printf("\nTrying regular call (Video only)...\n");
		REGULAR_CALL_FLOW(0, 4000);
	}
	
#undef REGULAR_CALL_FLOW
    
    if (TEST_REJECT) { /* Alice calls; Bob rejects */
        printf("\nTrying reject flow...\n");
        
        memset(&AliceCC, 0, sizeof(CallControl));
        memset(&BobCC, 0, sizeof(CallControl));
        
        {
            TOXAV_ERR_CALL rc;
            toxav_call(AliceAV, 0, 48, 0, &rc);
            
            if (rc != TOXAV_ERR_CALL_OK) {
                printf("toxav_call failed: %d\n", rc);
                exit(1);
            }
        }
        
        while (!BobCC.incoming)
            iterate_tox(bootstrap, AliceAV, BobAV);
        
        /* Reject */
        {
            TOXAV_ERR_CALL_CONTROL rc;
            toxav_call_control(BobAV, 0, TOXAV_CALL_CONTROL_CANCEL, &rc);
            
            if (rc != TOXAV_ERR_CALL_CONTROL_OK) {
                printf("toxav_call_control failed: %d\n", rc);
                exit(1);
            }
        }
        
        while (AliceCC.state != TOXAV_CALL_STATE_END)
            iterate_tox(bootstrap, AliceAV, BobAV);
        
        printf("Success!\n");
    }
    
    if (TEST_CANCEL) { /* Alice calls; Alice cancels while ringing */
        printf("\nTrying cancel (while ringing) flow...\n");
        
        memset(&AliceCC, 0, sizeof(CallControl));
        memset(&BobCC, 0, sizeof(CallControl));
        
        {
            TOXAV_ERR_CALL rc;
            toxav_call(AliceAV, 0, 48, 0, &rc);
            
            if (rc != TOXAV_ERR_CALL_OK) {
                printf("toxav_call failed: %d\n", rc);
                exit(1);
            }
        }
        
        while (!BobCC.incoming)
            iterate_tox(bootstrap, AliceAV, BobAV);
        
        /* Cancel */
        {
            TOXAV_ERR_CALL_CONTROL rc;
            toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_CANCEL, &rc);
            
            if (rc != TOXAV_ERR_CALL_CONTROL_OK) {
                printf("toxav_call_control failed: %d\n", rc);
                exit(1);
            }
        }
        
        /* Alice will not receive end state */
        while (BobCC.state != TOXAV_CALL_STATE_END)
            iterate_tox(bootstrap, AliceAV, BobAV);
        
        printf("Success!\n");
    }
    
    if (TEST_MUTE_UNMUTE) { /* Check Mute-Unmute etc */
        printf("\nTrying mute functionality...\n");
        
        memset(&AliceCC, 0, sizeof(CallControl));
        memset(&BobCC, 0, sizeof(CallControl));
        
        /* Assume sending audio and video */
        {
            TOXAV_ERR_CALL rc;
            toxav_call(AliceAV, 0, 48, 1000, &rc);
            
            if (rc != TOXAV_ERR_CALL_OK) {
                printf("toxav_call failed: %d\n", rc);
                exit(1);
            }
        }
        
        while (!BobCC.incoming)
            iterate_tox(bootstrap, AliceAV, BobAV);
        
        /* At first try all stuff while in invalid state */
        assert(!toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_PAUSE, NULL));
        assert(!toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_RESUME, NULL));
        assert(!toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_TOGGLE_MUTE_AUDIO, NULL));
        assert(!toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_TOGGLE_MUTE_VIDEO, NULL));
        
        {
            TOXAV_ERR_ANSWER rc;
            toxav_answer(BobAV, 0, 48, 4000, &rc);
            
            if (rc != TOXAV_ERR_ANSWER_OK) {
                printf("toxav_answer failed: %d\n", rc);
                exit(1);
            }
        }
        
        iterate_tox(bootstrap, AliceAV, BobAV);
        
        /* Pause and Resume */
        printf("Pause and Resume\n");
        assert(toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_PAUSE, NULL));
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state == TOXAV_CALL_STATE_PAUSED);
        assert(toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_RESUME, NULL));
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state & (TOXAV_CALL_STATE_SENDING_A | TOXAV_CALL_STATE_SENDING_V));
        
        /* Mute/Unmute single */
        printf("Mute/Unmute single\n");
        assert(toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_TOGGLE_MUTE_AUDIO, NULL));
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state ^ TOXAV_CALL_STATE_RECEIVING_A);
        assert(toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_TOGGLE_MUTE_AUDIO, NULL));
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state & TOXAV_CALL_STATE_RECEIVING_A);
        
        /* Mute/Unmute both */
        printf("Mute/Unmute both\n");
        assert(toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_TOGGLE_MUTE_AUDIO, NULL));
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state ^ TOXAV_CALL_STATE_RECEIVING_A);
        assert(toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_TOGGLE_MUTE_VIDEO, NULL));
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state ^ TOXAV_CALL_STATE_RECEIVING_V);
        assert(toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_TOGGLE_MUTE_AUDIO, NULL));
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state & TOXAV_CALL_STATE_RECEIVING_A);
        assert(toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_TOGGLE_MUTE_VIDEO, NULL));
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state & TOXAV_CALL_STATE_RECEIVING_V);
        
        {
            TOXAV_ERR_CALL_CONTROL rc;
            toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_CANCEL, &rc);
            
            if (rc != TOXAV_ERR_CALL_CONTROL_OK) {
                printf("toxav_call_control failed: %d\n", rc);
                exit(1);
            }
        }
        
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state == TOXAV_CALL_STATE_END);
        
        printf("Success!\n");
    }
    
    if (TEST_TRANSFER_A) { /* Audio encoding/decoding and transfer */
        SNDFILE* af_handle;
        SF_INFO af_info;
        
		printf("\nTrying audio enc/dec...\n");
		
		memset(&AliceCC, 0, sizeof(CallControl));
        memset(&BobCC, 0, sizeof(CallControl));
        
        { /* Call */
            TOXAV_ERR_CALL rc;
            toxav_call(AliceAV, 0, 48, 0, &rc);
            
            if (rc != TOXAV_ERR_CALL_OK) {
                printf("toxav_call failed: %d\n", rc);
                exit(1);
            }
        }
        
        while (!BobCC.incoming)
            iterate_tox(bootstrap, AliceAV, BobAV);
		
		{ /* Answer */
            TOXAV_ERR_ANSWER rc;
            toxav_answer(BobAV, 0, 64, 0, &rc);
            
            if (rc != TOXAV_ERR_ANSWER_OK) {
                printf("toxav_answer failed: %d\n", rc);
                exit(1);
            }
        }
        
        iterate_tox(bootstrap, AliceAV, BobAV);
		
        /* Open audio file */
        af_handle = sf_open(af_name, SFM_READ, &af_info);
        if (af_handle == NULL)
        {
            printf("Failed to open the file.\n");
            exit(1);
        }
        ALCdevice* audio_out_device = open_audio_device(audio_out_dev_name);
        
        int16_t PCM[5760];
        
        time_t start_time = time(NULL);
        time_t expected_time = af_info.frames / af_info.samplerate + 2;
        
        
        /* Start decode thread */
        struct toxav_thread_data data = { 
            .AliceAV = AliceAV,
            .BobAV = BobAV,
            .sig = 0
        };
        
        pthread_t dect;
        pthread_create(&dect, NULL, iterate_toxav, &data);
        pthread_detach(dect);
        
        printf("Sample rate %d\n", af_info.samplerate);
		while ( start_time + expected_time > time(NULL) ) {
            int frame_size = (af_info.samplerate * audio_frame_duration / 1000) * af_info.channels;
            
            int64_t count = sf_read_short(af_handle, PCM, frame_size);
            if (count > 0) {
//                 t_toxav_receive_audio_frame_cb(AliceAV, 0, PCM, count, af_info.channels, af_info.samplerate, NULL);
                TOXAV_ERR_SEND_FRAME rc;
                if (toxav_send_audio_frame(AliceAV, 0, PCM, count, af_info.channels, af_info.samplerate, &rc) == false) {
                    printf("Error sending frame of size %ld: %d\n", count, rc);
                    exit(1);
                }
            }
            iterate_tox(bootstrap, AliceAV, BobAV);
            c_sleep(audio_frame_duration);
		}
    
        
		printf("Played file in: %lu\n", time(NULL) - start_time);
        
        alcCloseDevice(audio_out_device);
        sf_close(af_handle);
		
		{ /* Hangup */
            TOXAV_ERR_CALL_CONTROL rc;
            toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_CANCEL, &rc);
            
            if (rc != TOXAV_ERR_CALL_CONTROL_OK) {
                printf("toxav_call_control failed: %d\n", rc);
                exit(1);
            }
        }
        
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state == TOXAV_CALL_STATE_END);
		
        /* Stop decode thread */
        data.sig = -1;
        while(data.sig != 1)
            pthread_yield();
        
		printf("Success!");
	}
	
	if (TEST_TRANSFER_V) {
        printf("\nTrying video enc/dec...\n");
        
        memset(&AliceCC, 0, sizeof(CallControl));
        memset(&BobCC, 0, sizeof(CallControl));
        
        { /* Call */
            TOXAV_ERR_CALL rc;
            toxav_call(AliceAV, 0, 0, 5000000, &rc);
            
            if (rc != TOXAV_ERR_CALL_OK) {
                printf("toxav_call failed: %d\n", rc);
                exit(1);
            }
        }
        
        while (!BobCC.incoming)
            iterate_tox(bootstrap, AliceAV, BobAV);
        
        { /* Answer */
            TOXAV_ERR_ANSWER rc;
            toxav_answer(BobAV, 0, 0, 5000000, &rc);
            
            if (rc != TOXAV_ERR_ANSWER_OK) {
                printf("toxav_answer failed: %d\n", rc);
                exit(1);
            }
        }
        
        iterate_tox(bootstrap, AliceAV, BobAV);
        
        /* Start decode thread */
        struct toxav_thread_data data = { 
            .AliceAV = AliceAV, 
            .BobAV = BobAV, 
            .sig = 0
        };
        
        pthread_t dect;
        pthread_create(&dect, NULL, iterate_toxav, &data);
        pthread_detach(dect);
        
        CvCapture* capture = cvCreateFileCapture(vf_name);
        if (!capture) {
            printf("Failed to open video file: %s\n", vf_name);
            exit(1);
        }
        
        time_t start_time = time(NULL);
        while(start_time + 90 > time(NULL)) {
            IplImage* frame = cvQueryFrame( capture );
            if (!frame)
                break;
            
            send_opencv_img(AliceAV, 0, frame);
            iterate_tox(bootstrap, AliceAV, BobAV);
            c_sleep(video_frame_duration);
        }
        
        cvReleaseCapture(&capture);
        
        { /* Hangup */
            TOXAV_ERR_CALL_CONTROL rc;
            toxav_call_control(AliceAV, 0, TOXAV_CALL_CONTROL_CANCEL, &rc);
            
            if (rc != TOXAV_ERR_CALL_CONTROL_OK) {
                printf("toxav_call_control failed: %d\n", rc);
                exit(1);
            }
        }
        
        iterate_tox(bootstrap, AliceAV, BobAV);
        assert(BobCC.state == TOXAV_CALL_STATE_END);
        
        /* Stop decode thread */
        printf("Stopping decode thread\n");
        data.sig = -1;
        while(data.sig != 1) 
            pthread_yield();
        
        printf("Success!");
    }
    
    
    Tox* Alice = toxav_get_tox(AliceAV);
    Tox* Bob = toxav_get_tox(BobAV);
    toxav_kill(BobAV);
    toxav_kill(AliceAV);
    tox_kill(Bob);
    tox_kill(Alice);
    tox_kill(bootstrap);
    
    printf("\nTest successful!\n");
    return 0;
}
