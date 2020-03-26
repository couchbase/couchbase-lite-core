pipeline {
    agent none
    options {
        timeout(time: 30, unit: 'MINUTES') 
    }
    stages {
        stage("Build Mobile") {
            parallel {
                stage("Android") {
                    agent { label 'mobile-mac-mini' }
                    environment {
                       BRANCH = "${BRANCH_NAME}"
                    }
                    steps {
                        sh 'jenkins/jenkins_android.sh $HOME/Library/Developer/Xamarin/android-sdk-macosx'
                    }
                }
                stage("iOS") {
                    agent { label 'mobile-mac-mini' }
                    environment {
                        BRANCH = "${BRANCH_NAME}"
                        KEYCHAIN_PWD = credentials("mobile-mac-mini-keychain")
                    }
                    steps {
                        sh 'jenkins/jenkins_ios.sh'
                    }
                }
                stage("UWP") {
                    agent { label 'couchbase-lite-net-validation' }
                    environment {
                        BRANCH = "${BRANCH_NAME}"
                    }
                    steps {
                        powershell 'jenkins\\jenkins_uwp.ps1'
                    }
                }
            }
        }

        stage("Build and Test Desktop") {
            parallel {
                stage("Windows") {
                   agent { label 'couchbase-lite-net-validation' }
                   environment {
                       BRANCH = "${BRANCH_NAME}"
                   }
                   steps {
                       powershell 'jenkins\\jenkins_win.ps1'
                   }
                }
                stage("macOS") {
                    agent { label 'mobile-mac-mini'  }
                    environment {
                        BRANCH = "${BRANCH_NAME}"
                    }
                    steps {
                        sh 'jenkins/jenkins_unix.sh'
                    }
                }
                stage("Linux") {
                    agent { label 's61113u16 (litecore)' }
                    environment {
                       BRANCH = "${BRANCH_NAME}"
                       CC = "gcc-7"
                       CXX = "g++-7"
                    }
                    steps {
                        sh 'jenkins/jenkins_unix.sh'
                    }
                }
            }
        }
    }
}
