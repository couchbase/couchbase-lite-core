pipeline {
    agent none
    options {
        timeout(time: 60, unit: 'MINUTES') 
    }
    stages {
        stage("Build Mobile") {
            parallel {
                stage("Android") {
                    agent { label 'mobile-lite-android' }
                    environment {
                       BRANCH = "${BRANCH_NAME}"
                    }
                    steps {
                        sh 'jenkins/jenkins_android.sh $HOME/jenkins/tools/android-sdk'
                    }
                }
                stage("iOS") {
                    agent { label 'sonoma' }
                    environment {
                        BRANCH = "${BRANCH_NAME}"
                        KEYCHAIN_PWD = credentials("keychain-password")
                    }
                    steps {
                        sh 'jenkins/jenkins_ios.sh'
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
                    agent { label 'sonoma'  }
                    environment {
                        BRANCH = "${BRANCH_NAME}"
                        GH_PAT = credentials("cbl-bot-github-pat")
                    }
                    steps {
                        sh 'build_cmake/scripts/cover_macos.sh --export-results --push'
                    }
                }
                stage("Linux") {
                    agent { label 's61113u16 (litecore)' }
                    environment {
                       BRANCH = "${BRANCH_NAME}"
                    }
                    steps {
                        sh 'jenkins/jenkins_unix.sh'
                    }
                }
            }
        }
    }
}
