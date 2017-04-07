cmake_minimum_required(VERSION 3.4.1)

project (LiteCoreJNI)

set(TOP ${PROJECT_SOURCE_DIR}/..)

find_package(Java COMPONENTS Development)
include (UseJava)

# Android does not work with FindJNI
if (NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    MESSAGE( STATUS "!!!!!!!CMAKE_SYSTEM_NAME: "         ${CMAKE_SYSTEM_NAME} )
    MESSAGE( STATUS "!!!!!!!ANDROID: "         ${ANDROID} )
    find_package(JNI)
endif()

MESSAGE( STATUS "Java_FOUND: "            ${Java_FOUND} )
MESSAGE( STATUS "Java_INCLUDE_DIRS: "     ${Java_INCLUDE_DIRS} )
MESSAGE( STATUS "JAVA_INCLUDE_PATH: "     ${JAVA_INCLUDE_PATH} )
MESSAGE( STATUS "JAVA_INCLUDE_PATH2: "    ${JAVA_INCLUDE_PATH2} )
MESSAGE( STATUS "Java_JAVAH_EXECUTABLE: " ${Java_JAVAH_EXECUTABLE} )


# configure import libs
set(OPENSSL_LIB_DIR ${TOP}/vendor/openssl/libs/android/clang)


#
# Generate JNI header files
#
set(JNI_CLASSPATH  ${PROJECT_SOURCE_DIR}/src)
set(JNI_OUTPUT_DIR ${PROJECT_SOURCE_DIR}/jni)
set(JNI_CLASSES
    com.couchbase.litecore.C4BlobKey
    com.couchbase.litecore.C4BlobStore
    com.couchbase.litecore.C4Query
    com.couchbase.litecore.C4QueryEnumerator
    com.couchbase.litecore.C4BlobReadStream
    com.couchbase.litecore.C4BlobWriteStream
    com.couchbase.litecore.Database
    com.couchbase.litecore.Document
    com.couchbase.litecore.DocumentIterator
    com.couchbase.litecore.LiteCoreException
    com.couchbase.litecore.Logger
    com.couchbase.litecore.fleece.FLArray
    com.couchbase.litecore.fleece.FLArrayIterator
    com.couchbase.litecore.fleece.FLDict
    com.couchbase.litecore.fleece.FLDictIterator
    com.couchbase.litecore.fleece.FLEncoder
    com.couchbase.litecore.fleece.FLSliceResult
    com.couchbase.litecore.fleece.FLValue)

create_javah(TARGET JNIHeaders
    CLASSES ${JNI_CLASSES}
    CLASSPATH ${JNI_CLASSPATH}
    OUTPUT_DIR ${JNI_OUTPUT_DIR}
    DEPENDS ${JNI_CLASSPATH})

#
# JNI for litecore - shared library including litecore static library
#

# jni.h and jni_md.h for non-Android
if (NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    include_directories(
        ${JAVA_INCLUDE_PATH}
        ${JAVA_INCLUDE_PATH2}
)
endif()

include_directories(
	${TOP}/C/include/
	${TOP}/LiteCore/Storage/
    ${TOP}/LiteCore/Support/
    ${TOP}/vendor/fleece/Fleece/)

aux_source_directory(./jni  JNI_SRC)
add_library(LiteCoreJNI SHARED ${JNI_SRC})

# set dependency
add_dependencies(LiteCoreJNI JNIHeaders)

#
# crypto
#
add_library(crypto STATIC IMPORTED)
set_target_properties(crypto PROPERTIES IMPORTED_LOCATION
        ${OPENSSL_LIB_DIR}/${ANDROID_ABI}/libcrypto.a)



if (NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    target_link_libraries(  LiteCoreJNI
                            LiteCoreStatic
                            FleeceStatic
                            sqlite3
                            crypto
                            SQLite3_UnicodeSN)
else()
    
    #find_library(log-lib log)

	target_link_libraries(	LiteCoreJNI
                        	LiteCoreStatic
                        	FleeceStatic
                        	sqlite3
                            crypto
                        	SQLite3_UnicodeSN
                        	"log")


endif()