/*
 * Copyright (c) 2025  IBM Corporation.  All rights reserved.
 *
 * Using the Jenkins 'Scripted Pipeline' syntax
 */


import groovy.transform.Field
import java.util.regex.Pattern;
import java.util.regex.Matcher;
import org.jenkinsci.plugins.pipeline.modeldefinition.Utils

// -------------------------------------------------
// Testing
// -------------------------------------------------
// Support CI/CD directory

def dir_ci_scripts='kernels'
def ftp3accesscred = ''
def tokencred = ''
def gsausercred = ''
def githubusercred = ''
def cloudkey = ''
def didSucceed = 1
def GIT_COMMIT_MSG= ''
def DISTRO='rhel101'

def run_in_shell(String cmd, pipefail=true, debug=false){
  if (pipefail) {
      println "==============================\n" +
              "Running Command with pipefail:\n" +
              "  " + cmd + "\n" +
              "=============================="
      if (debug) {
          sh('#!/bin/bash -x\n' + "set -o pipefail; " + cmd)
      }
      else {
          sh('#!/bin/bash \n' + "set -o pipefail; " + cmd)
      }
  }
  else {
      println "================\n" +
              "Running Command:\n" +
              "  " + cmd + "\n" +
              "================"
      if (debug) {
          sh('#!/bin/bash -x\n' + cmd)
      }
      else {
          sh('#!/bin/bash \n' + cmd)
      }
  }
}

def REPO_URL='icr.io/sys-kernelbackport'

node("powerpc") {
   
    stage("Init") {
        script {
            withCredentials([
                string(credentialsId: 'token', variable: 'token'),
                string(credentialsId: 'ftp3access', variable: 'ftp3access'),
                string(credentialsId: 'gsauser', variable: 'gsauser'),
                string(credentialsId: 'githubuser', variable: 'githubuser'),
                string(credentialsId: 'ibmcloudkey', variable: 'ibmcloudkey'),]){
                gsausercred= gsauser
                githubusercred= githubuser
                ftp3accesscred= ftp3access
                tokencred = token
                cloudkey = ibmcloudkey
            }
        }
        dir_ci_scripts_full_path  = "" + env.WORKSPACE + "/" + dir_ci_scripts
	    withCredentials([gitUsernamePassword(credentialsId: 'github-token', gitToolName: 'Default')]) {
            dir("${env.WORKSPACE}/" + dir_ci_scripts) {
                checkout scm
		script {
                    GIT_COMMIT_MSG = sh (script: 'git log -1 --pretty=format:"%s" ${GIT_COMMIT}', returnStdout: true).trim()
        	}
            }
        }
	try {
        	run_in_shell("python3 -m pip install github-api-python git-python PyGithub")
		didSucceed = 0
	}catch (Exception e) {
                didSucceed = 1
                println "caught a exception"
        }
    }
    
    stage("Pull image"){
        dir(dir_ci_scripts_full_path) {
	   try {
            run_in_shell("podman login -u iamapikey -p " + cloudkey+ " " + REPO_URL)
	    run_in_shell("podman pull "+REPO_URL+"/"+ DISTRO+":latest")
	    didSucceed = 0
	}catch (Exception e) {
                didSucceed = 1
	}
        }
    }

    stage("update kernel"){
        dir(dir_ci_scripts_full_path) {
	    if (didSucceed == 0){
	    try {
            	println githubusercred
            	println gsausercred
            	println GIT_COMMIT_MSG
		run_in_shell("sh jenkinsbuild.sh " +  REPO_URL +" " +DISTRO+" "+ "'Praveen K Pandey'" + " "+ "praveen.pandey@in.ibm.com" + " "+ ftp3accesscred)
            	didSucceed = 0
		cleanWs()
	}catch (Exception e) {
		didSucceed = 1
		cleanWs()
		println "caught a exception"
        }
	}
	}
    }
  
}
