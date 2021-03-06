JENKINS_URL = 'https://jenkins.below92.com/'
GITHUB_HOOK_URL = 'https://github.com/worklist/hifi/'
GIT_REPO_URL = 'git@github.com:worklist/hifi.git'
HIPCHAT_ROOM = 'High Fidelity'

def hifiJob(String targetName, Boolean deploy) {
    job {
        name "hifi-${targetName}"
        logRotator(7, -1, -1, -1)
        
        scm {
            git(GIT_REPO_URL, 'master') { node ->
                node << {
                    includedRegions "${targetName}/.*\nlibraries/.*"
                    useShallowClone true
                }
            }
        }
       
        configure { project ->
            project / 'properties' << {
                'com.coravy.hudson.plugins.github.GithubProjectProperty' {
                    projectUrl GITHUB_HOOK_URL
                }
                
                'jenkins.plugins.hipchat.HipChatNotifier_-HipChatJobProperty' {
                    room HIPCHAT_ROOM
                } 
                
                'hudson.plugins.buildblocker.BuildBlockerProperty' {
                    useBuildBlocker true
                    blockingJobs 'hifi--seed'
                }         
            }
            
            project / 'triggers' << 'com.cloudbees.jenkins.GitHubPushTrigger' {
                spec ''
            }
        } 
        
        configure cmakeBuild(targetName, 'make install')
        
        if (deploy) {
            publishers {            
                publishScp("${ARTIFACT_DESTINATION}") {
                    entry("**/build/${targetName}/${targetName}", "deploy/${targetName}")
                }
            }
        }
        
        configure { project ->
            
            project / 'publishers' << {
                if (deploy) {
                    'hudson.plugins.postbuildtask.PostbuildTask' {
                        'tasks' {
                            'hudson.plugins.postbuildtask.TaskProperties' {
                                logTexts {
                                    'hudson.plugins.postbuildtask.LogProperties' {
                                        logText '.'
                                        operator 'AND'
                                    }
                                }
                                EscalateStatus true
                                RunIfJobSuccessful true
                                script "curl -d 'action=deploy&role=highfidelity-live&revision=${targetName}' https://${ARTIFACT_DESTINATION}"
                            }
                        }
                    }
                }
                
                'jenkins.plugins.hipchat.HipChatNotifier' {
                    jenkinsUrl JENKINS_URL
                    authToken "${HIPCHAT_AUTH_TOKEN}"
                    room HIPCHAT_ROOM
                }
            }
        }
    }
}

static Closure cmakeBuild(srcDir, instCommand) {
    return { project ->
        project / 'builders' / 'hudson.plugins.cmake.CmakeBuilder' {
            sourceDir '.'
            buildDir 'build'
            installDir ''
            buildType 'RelWithDebInfo'
            generator 'Unix Makefiles'
            makeCommand "make ${srcDir}"
            installCommand instCommand
            preloadScript ''
            cmakeArgs ''
            projectCmakePath '/usr/local/bin/cmake'
            cleanBuild 'false'
            cleanInstallDir 'false'
            builderImpl ''
        }
    }
}

def targets = [
    'animation-server':true,
    'assignment-server':true,
    'assignment-client':true,
    'domain-server':true,
    'eve':true,
    'pairing-server':true,
    'space-server':true,
    'voxel-server':true,
]

/* setup all of the target jobs to use the above template */
for (target in targets) {
    queue hifiJob(target.key, target.value)
}

/* setup the OS X interface builds */
interfaceOSXJob = hifiJob('interface', false)
interfaceOSXJob.with {
    name 'hifi-interface-osx'
    
    scm {
        git(GIT_REPO_URL, 'stable') { node ->
            node << {
                includedRegions "interface/.*\nlibraries/.*"
                useShallowClone true
            }
        }
    }
    
    configure { project ->
        project << {
            assignedNode 'interface-mini'
            canRoam false
        } 
    }
}

queue interfaceOSXJob

/* setup the parametrized build job for builds from jenkins */
parameterizedJob = hifiJob('$TARGET', true)
parameterizedJob.with {
    name 'hifi-branch-deploy'
    parameters {
        stringParam('GITHUB_USER', '', "Specifies the name of the GitHub user that we're building from.")
        stringParam('GIT_BRANCH', '', "Specifies the specific branch to build and deploy.")
        stringParam('HOSTNAME', 'devel.highfidelity.io', "Specifies the hostname to deploy against.")
        stringParam('TARGET', '', "What server to build specifically")
    }   
    scm {
        git('git@github.com:/$GITHUB_USER/hifi.git', '$GIT_BRANCH') { node ->
            node << {
                wipeOutWorkspace true
                useShallowClone true
            } 
            
        }
    } 
    configure { project ->
        def curlCommand = 'curl -d action=hifidevgrid -d "hostname=$HOSTNAME" ' +
                          '-d "github_user=$GITHUB_USER" -d "build_branch=$GIT_BRANCH" ' +
                          "-d \"revision=\$TARGET\" https://${ARTIFACT_DESTINATION}"
        
        (project / publishers / 'hudson.plugins.postbuildtask.PostbuildTask' / 
            tasks / 'hudson.plugins.postbuildtask.TaskProperties' / script).setValue(curlCommand)
    }
}

doxygenJob = hifiJob('docs', false)
doxygenJob.with {
    scm {
        git(GIT_REPO_URL, 'master') { node ->
            node << {
                useShallowClone true
            }
        }
    }
    
    configure { project ->
        (project / builders).setValue('')
    }
    
    steps {
        shell('doxygen')
    }
}

queue doxygenJob
