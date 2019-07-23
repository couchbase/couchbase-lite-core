pipeline {
    agent any
    stages {
        stage("Build and Test") {
            parallel {
                stage("Windows") {
                    agent { label 's61114win10_(litecore)' }
                    steps {
                        powershell(returnStatus: true, script: 'jenkins\\jenkins_win.ps1')
                    }
                }
            }
        }
    }
}