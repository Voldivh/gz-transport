/*
 * Copyright (C) 2014 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#ifndef GZ_TRANSPORT_TEST_UTILS_HH_
#define GZ_TRANSPORT_TEST_UTILS_HH_

#include <climits>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <signal.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

#include "gz/transport/Helpers.hh"

namespace testing
{
  /// \brief Join _str1 and _str2 considering both as storing system paths.
  /// \param[in] _str1 string containing a path.
  /// \param[in] _str2 string containing a path.
  /// \return The string representation of the union of two paths.
  std::string portablePathUnion(const std::string &_str1,
                                const std::string &_str2)
  {
    return (std::filesystem::path(_str1) / std::filesystem::path(_str2)).string();
  }

#ifdef _WIN32
  using forkHandlerType = PROCESS_INFORMATION;
#else
  using forkHandlerType = pid_t;
#endif

  /// \brief create a new process and run command on it. This function is
  /// implementing the creation of a new process on both Linux (fork) and
  /// Windows (CreateProcess) and the execution of the command provided.
  /// \param[in] _command The full system path to the binary to run into the
  /// new process.
  /// \param[in] _partition Name of the Gazebo partition (GZ_PARTITION)
  /// \param[in] _username Username for authentication
  /// (GZ_TRANSPORT_USERNAME)
  /// \param[in] _password Password for authentication
  /// (GZ_TRANSPORT_PASSWORD)
  /// \return On success, the PID of the child process is returned in the
  /// parent, an 0 is returned in the child. On failure, -1 is returned in the
  /// parent and no child process is created.
  forkHandlerType forkAndRun(const char *_command, const char *_partition,
      const char *_username = nullptr, const char *_password = nullptr)
  {
#ifdef _WIN32
    STARTUPINFO info= {sizeof(info)};
    PROCESS_INFORMATION processInfo;

    char cmd[500];
    // We should put quotes around the _command string to make sure we are
    // robust to file paths that contain spaces.
    gz_strcpy(cmd, "\"");
    gz_strcat(cmd, _command);
    gz_strcat(cmd, "\"");
    gz_strcat(cmd, " ");
    gz_strcat(cmd, _partition);

    if (_username && _password)
    {
      gz_strcat(cmd, " ");
      gz_strcat(cmd, _username);
      gz_strcat(cmd, " ");
      gz_strcat(cmd, _password);
    }

    // We set the first argument to NULL, because we want the behavior that
    // CreateProcess exhibits when the first argument is NULL: i.e. Windows will
    // automatically add the .exe extension onto the filename. When the first
    // argument is non-NULL, it will not automatically add the extension, which
    // makes more work for us.
    //
    // It should also be noted that the lookup behavior for the application is
    // different when the first argument is non-NULL, so we should take that
    // into consideration when determining what to put into the first and second
    // arguments of CreateProcess.
    if (!CreateProcess(NULL, const_cast<LPSTR>(cmd), NULL, NULL,
          TRUE, 0, NULL, NULL, &info, &processInfo))
    {
      std::cerr << "CreateProcess call failed: " << cmd << std::endl;
    }

    return processInfo;
#else
    pid_t pid = fork();

    if (pid == 0)
    {
      if (_username && _password)
      {
        if (execl(_command, _command, _partition, _username, _password,
              reinterpret_cast<char *>(0)) == -1)
        {
          std::cerr << "Error running execl call: " << _command << std::endl;
        }
      }
      else
      {
        if (execl(_command, _command, _partition,
              reinterpret_cast<char *>(0)) == -1)
        {
          std::cerr << "Error running execl call: " << _command << std::endl;
        }
      }
    }

    return pid;
#endif
  }

  /// \brief Wait for the end of a process and handle the termination
  /// \param[in] pi Process handler of the process to wait for
  /// (PROCESS_INFORMATION in windows or forkHandlerType in UNIX).
  void waitAndCleanupFork(const forkHandlerType pi)
  {
#ifdef _WIN32
    // Wait until child process exits.
    WaitForSingleObject(pi.hProcess, INFINITE);

    // Close process and thread handler.
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    // Wait for the child process to return.
    int status;
    waitpid(pi, &status, 0);
    if (status == -1)
      std::cerr << "Error while running waitpid" << std::endl;
#endif
  }

  /// \brief Send a termination signal to the process handled by pi.
  /// \param[in] pi Process handler of the process to stop
  /// (PROCESS_INFORMATION in windows or forkHandlerType in UNIX).
  void killFork(const forkHandlerType pi)
  {
#ifdef _WIN32
    // TerminateProcess return 0 on error
    if (TerminateProcess(pi.hProcess, 0) == 0)
      std::cerr << "Error running TerminateProcess: " << GetLastError();
#else
    kill(pi, SIGTERM);
#endif
  }

  /// \brief Get a random number based on an integer converted to string.
  /// \return A random integer converted to string.
  std::string getRandomNumber()
  {
    // Initialize random number generator.
    uint32_t seed = std::random_device {}();
    std::mt19937 randGenerator(seed);

    // Create a random number based on an integer converted to string.
    std::uniform_int_distribution<int32_t> d(0, INT_MAX);

    return std::to_string(d(randGenerator));
  }
}

#endif  // header guard
