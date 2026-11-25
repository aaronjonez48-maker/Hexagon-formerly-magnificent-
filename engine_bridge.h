#pragma once
#include <string>
#include <vector>

// Bridge between the UI (presto_ui.cpp) and the backend engines.
void InitEngineModules();
void LoadModelFile(const std::string& path);
void UpdateRigPose(int frameNumber, int activeTrackIndex, float value);

// Read-only view of the engine rig (so the UI can draw it)
int  EngineJointCount();
int  EngineJointParent(int i);
bool EngineJointPos(int i, float* xyz);   // world position, xyz[3]
void RigAsciiPreview();

// Long-running self-tests / renders (run from the menu on a worker thread)
int RunTimelineSelfTest();
int RunSfmRender();
