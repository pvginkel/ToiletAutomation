// Builds the ToiletAutomation firmware and uploads it over the air through IoTSupport, which tells
// every device of the model toilet_automation to install it.
//
// Controller config:
//   - Job: Firmware/ToiletAutomation
//   - SCM: pvginkel/ToiletAutomation, branch main
//   - Script Path: Jenkinsfile

library identifier: 'JenkinsPipelineUtils', changelog: false

pipeline {
    agent {
        kubernetes {
            inheritFrom 'jenkins-agent-large'
            yamlMergeStrategy merge()
            yaml podYaml(images: [[image: 'espressif/idf:v5.5.3', name: 'idf']])
        }
    }

    options {
        // Without abortPrevious: an upload cut off by an abort leaves the devices on two firmware
        // versions.
        disableConcurrentBuilds()
        skipDefaultCheckout()
        timeout(time: 60, unit: 'MINUTES')
        timestamps()
    }

    triggers {
        githubPush()
    }

    stages {
        stage('Checkout') {
            steps {
                dir('ToiletAutomation') {
                    checkout scm
                }
                dir('esp-libs') {
                    git url: 'https://github.com/pvginkel/esp-libs.git', branch: 'main',
                        credentialsId: '5f6fbd66-b41c-405f-b107-85ba6fd97f10'
                }
            }
        }

        stage('Build firmware') {
            steps {
                script {
                    espFirmware.build(dir: 'ToiletAutomation')
                }
            }
        }

        stage('Deploy firmware') {
            steps {
                script {
                    espFirmware.upload(dir: 'ToiletAutomation')
                }
            }
        }
    }

    post {
        aborted {
            script {
                notify.error("${env.JOB_NAME} #${env.BUILD_NUMBER} aborted (timeout or hand)")
            }
        }
    }
}
