#include <jni.h>
#include <string>

// ----- Catch -----
#define CATCH_CONFIG_RUNNER
#include "catch.hpp"
#include "CaseListReporter.hh"

#include "c4Test.hh"

// ----- Android Native Logging -----
#include <android/log.h>
static int pfd[2];
static pthread_t thr = 0;
static const char *TAG = "lite-core::C4Tests";

static void *thread_func(void*) {
    ssize_t rdsz;
    char buf[128];
    while((rdsz = read(pfd[0], buf, sizeof buf - 1)) > 0) {
        if(buf[rdsz - 1] == '\n') --rdsz;
        buf[rdsz] = 0;  /* add null-terminator */
        __android_log_write(ANDROID_LOG_ERROR, TAG, buf);
    }
    return 0;
}

int start_logger() {
    // make stdout line-buffered and stderr unbuffered
    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);

    // create the pipe and redirect stdout and stderr
    pipe(pfd);
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);

    // spawn the logging thread
    if (pthread_create(&thr, 0, thread_func, 0) == -1)
        return -1;

    pthread_detach(thr);
    return 0;
}

void close_logger(){
    ::fclose(stdout); // send EOF
}

// ----- JNI & Run CppTests -----
extern "C"
jstring
Java_com_couchbase_lite_c4tests_MainActivity_runC4Tests(
        JNIEnv *env,
        jobject jobj,
        jstring jpath) {

    start_logger();

    // @see FilePath FilePath::tempDirectory() in FilePath.cc
    const char* path = env->GetStringUTFChars(jpath , NULL) ;
    setenv("TMPDIR", path, 1);

    // overwite the test data file directory.
    C4Test::sFixturesDir = std::string(path) + "/C/tests/data/";

    const int argc = 3;
    const char *argv[argc] = {(char*)"C4Tests", (char*)"-r", (char*)"list"};
    int result = Catch::Session().run(argc, argv);
    close_logger();

    char buff[1024];
    sprintf(buff, "CppTests: results=%d", result);
    return env->NewStringUTF(buff);
}