pipeline {
    agent any
    stages {
        stage("Build and Test") {
            parallel {
                stage("Windows") {
                   agent { label 's61114win10_(litecore)' }
                   environment {
                       BRANCH = "${BRANCH_NAME}"
                   }
                   steps {
                       def retVal = powershell(returnStatus: true, script: 'jenkins\\jenkins_win.ps1')
                       if(retVal != 0) {
                           error("Windows stage failed")
                       }
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
                       CC = "clang"
                       CXX = "clang++"
                    }
                    steps {
                        sh 'jenkins/jenkins_unix.sh'
                    }
                }
            }
        }
    }
}
