pipeline {
    agent none
    options {
        timeout(time: 60, unit: 'MINUTES') 
    }
    stages {
        

        stage("Build and Test Desktop") {
            parallel {
                stage("macOS") {
                    agent { label 'mobile-mac-mini'  }
                    environment {
                        BRANCH = "${BRANCH_NAME}"
                        GH_PAT = credentials("cbl-bot-github-pat")
                    }
                    steps {
                        sh 'build_cmake/scripts/cover_macos.sh --export-results --push'
                    }
                }
            }
        }
    }
}
