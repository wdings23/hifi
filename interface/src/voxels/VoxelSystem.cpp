//
//  VoxelSystem.cpp
//  interface/src/voxels
//
//  Created by Philip on 12/31/12.
//  Copyright 2012 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//


#include <cstring>
#include <cmath>
#include <iostream> // to load voxels from file
#include <fstream> // to load voxels from file

#include <OctalCode.h>
#include <PacketHeaders.h>
#include <PerfStat.h>
#include <SharedUtil.h>
#include <NodeList.h>

#include <glm/gtc/type_ptr.hpp>

#include "Application.h"
#include "InterfaceConfig.h"
#include "Menu.h"
#include "renderer/ProgramObject.h"
#include "VoxelConstants.h"
#include "VoxelSystem.h"

const bool VoxelSystem::DONT_BAIL_EARLY = false;

float identityVerticesGlobalNormals[] = { 0,0,0, 1,0,0, 1,1,0, 0,1,0, 0,0,1, 1,0,1, 1,1,1, 0,1,1 };

float identityVertices[] = { 0,0,0, 1,0,0, 1,1,0, 0,1,0, 0,0,1, 1,0,1, 1,1,1, 0,1,1, //0-7
                             0,0,0, 1,0,0, 1,1,0, 0,1,0, 0,0,1, 1,0,1, 1,1,1, 0,1,1, //8-15
                             0,0,0, 1,0,0, 1,1,0, 0,1,0, 0,0,1, 1,0,1, 1,1,1, 0,1,1 }; // 16-23

GLfloat identityNormals[] = { 0,0,-1, 0,0,-1, 0,0,-1, 0,0,-1,
                              0,0,+1, 0,0,+1, 0,0,+1, 0,0,+1,
                              0,-1,0, 0,-1,0, 0,+1,0, 0,+1,0,
                              0,-1,0, 0,-1,0, 0,+1,0, 0,+1,0,
                              -1,0,0, +1,0,0, +1,0,0, -1,0,0,
                              -1,0,0, +1,0,0, +1,0,0, -1,0,0 };

GLubyte identityIndices[] = { 0,2,1,    0,3,2,    // Z-
                              8,9,13,   8,13,12,  // Y-
                              16,23,19, 16,20,23, // X-
                              17,18,22, 17,22,21, // X+
                              10,11,15, 10,15,14, // Y+
                              4,5,6,    4,6,7 };  // Z+

GLubyte identityIndicesTop[]    = {  2, 3, 7,  2, 7, 6 };
GLubyte identityIndicesBottom[] = {  0, 1, 5,  0, 5, 4 };
GLubyte identityIndicesLeft[]   = {  0, 7, 3,  0, 4, 7 };
GLubyte identityIndicesRight[]  = {  1, 2, 6,  1, 6, 5 };
GLubyte identityIndicesFront[]  = {  0, 2, 1,  0, 3, 2 };
GLubyte identityIndicesBack[]   = {  4, 5, 6,  4, 6, 7 };

static glm::vec3 grayColor = glm::vec3(0.3f, 0.3f, 0.3f);

static GLuint _voxelModelID = 0;
static GLuint _voxelModelIndicesID = 0;

VoxelSystem::VoxelSystem(float treeScale, int maxVoxels, VoxelTree* tree)
    : NodeData(),
    _treeScale(treeScale),
    _maxVoxels(maxVoxels),
    _initialized(false),
    _writeArraysLock(QReadWriteLock::Recursive),
    _readArraysLock(QReadWriteLock::Recursive),
    _inOcclusions(false),
    _showCulledSharedFaces(false),
    _usePrimitiveRenderer(false),
    _renderer(0),
    _drawHaze(false),
    _farHazeDistance(300.0f),
    _hazeColor(grayColor)
{

    _voxelsInReadArrays = _voxelsInWriteArrays = _voxelsUpdated = 0;
    _writeRenderFullVBO = true;
    _readRenderFullVBO = true;
    _tree = (tree) ? tree : new VoxelTree();

    _tree->getRoot()->setVoxelSystem(this);

    VoxelTreeElement::addDeleteHook(this);
    VoxelTreeElement::addUpdateHook(this);
    _abandonedVBOSlots = 0;
    _falseColorizeBySource = false;
    _dataSourceUUID = QUuid();
    _voxelServerCount = 0;

    _viewFrustum = Application::getInstance()->getViewFrustum();

    _useVoxelShader = true;
    _voxelsAsPoints = false;
    _voxelShaderModeWhenVoxelsAsPointsEnabled = false;

    _writeVoxelShaderData = NULL;
    _readVoxelShaderData = NULL;

    _readVerticesArray = NULL;
    _writeVerticesArray = NULL;
    _readColorsArray = NULL;
    _writeColorsArray = NULL;
    _writeVoxelDirtyArray = NULL;
    _readVoxelDirtyArray = NULL;

    _inSetupNewVoxelsForDrawing = false;
    _useFastVoxelPipeline = false;

    _culledOnce = false;
    _inhideOutOfView = false;

    _lastKnownVoxelSizeScale = DEFAULT_OCTREE_SIZE_SCALE;
    _lastKnownBoundaryLevelAdjust = 0;
}

void VoxelSystem::elementDeleted(OctreeElement* element) {
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;
    if (voxel->getVoxelSystem() == this) {
        if ((_voxelsInWriteArrays != 0) || _usePrimitiveRenderer) {
            forceRemoveNodeFromArrays(voxel);
        } else {
            if (Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings)) {
                qDebug("VoxelSystem::elementDeleted() while _voxelsInWriteArrays==0, is that expected? ");
            }
        }
    }
}

void VoxelSystem::setDisableFastVoxelPipeline(bool disableFastVoxelPipeline) {
    _useFastVoxelPipeline = !disableFastVoxelPipeline;
    setupNewVoxelsForDrawing();
}

void VoxelSystem::elementUpdated(OctreeElement* element) {
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;

    // If we're in SetupNewVoxelsForDrawing() or _writeRenderFullVBO then bail..
    if (!_useFastVoxelPipeline || _inSetupNewVoxelsForDrawing || _writeRenderFullVBO) {
        return;
    }

    if (voxel->getVoxelSystem() == this) {
        bool shouldRender = false; // assume we don't need to render it
        // if it's colored, we might need to render it!
        float voxelSizeScale = Menu::getInstance()->getVoxelSizeScale();
        int boundaryLevelAdjust = Menu::getInstance()->getBoundaryLevelAdjust();
        shouldRender = voxel->calculateShouldRender(_viewFrustum, voxelSizeScale, boundaryLevelAdjust);

        if (voxel->getShouldRender() != shouldRender) {
            voxel->setShouldRender(shouldRender);
        }

        if (!voxel->isLeaf()) {

            // As we check our children, see if any of them went from shouldRender to NOT shouldRender
            // then we probably dropped LOD and if we don't have color, we want to average our children
            // for a new color.
            int childrenGotHiddenCount = 0;
            for (int i = 0; i < NUMBER_OF_CHILDREN; i++) {
                VoxelTreeElement* childVoxel = voxel->getChildAtIndex(i);
                if (childVoxel) {
                    bool wasShouldRender = childVoxel->getShouldRender();
                    bool isShouldRender = childVoxel->calculateShouldRender(_viewFrustum, voxelSizeScale, boundaryLevelAdjust);
                    if (wasShouldRender && !isShouldRender) {
                        childrenGotHiddenCount++;
                    }
                }
            }
            if (childrenGotHiddenCount > 0) {
                voxel->calculateAverageFromChildren();
            }
        }

        const bool REUSE_INDEX = true;
        const bool DONT_FORCE_REDRAW = false;
        updateNodeInArrays(voxel, REUSE_INDEX, DONT_FORCE_REDRAW);
        _voxelsUpdated++;

        voxel->clearDirtyBit(); // clear the dirty bit, do this before we potentially delete things.

        setupNewVoxelsForDrawingSingleNode();
    }
}

// returns an available index, starts by reusing a previously freed index, but if there isn't one available
// it will use the end of the VBO array and grow our accounting of that array.
// and makes the index available for some other node to use
glBufferIndex VoxelSystem::getNextBufferIndex() {
    glBufferIndex output = GLBUFFER_INDEX_UNKNOWN;
    // if there's a free index, use it...
    if (_freeIndexes.size() > 0) {
        _freeIndexLock.lock();
        output = _freeIndexes.back();
        _freeIndexes.pop_back();
        _freeIndexLock.unlock();
    } else {
        output = _voxelsInWriteArrays;
        _voxelsInWriteArrays++;
    }
    return output;
}

// Release responsibility of the buffer/vbo index from the VoxelTreeElement, and makes the index available for some other node to use
// will also "clean up" the index data for the buffer/vbo slot, so that if it's in the middle of the draw range, the triangles
// will be "invisible"
void VoxelSystem::freeBufferIndex(glBufferIndex index) {
    if (_voxelsInWriteArrays == 0) {
        qDebug() << "freeBufferIndex() called when _voxelsInWriteArrays == 0!";
    }

    // make the index available for next node that needs to be drawn
    _freeIndexLock.lock();
    _freeIndexes.push_back(index);
    _freeIndexLock.unlock();

    // make the VBO slot "invisible" in case this slot is not used
    const glm::vec3 startVertex(FLT_MAX, FLT_MAX, FLT_MAX);
    const float voxelScale = 0;
    const nodeColor BLACK = {0, 0, 0, 0};
    updateArraysDetails(index, startVertex, voxelScale, BLACK);
}

// This will run through the list of _freeIndexes and reset their VBO array values to be "invisible".
void VoxelSystem::clearFreeBufferIndexes() {
    bool showWarnings = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showWarnings, "clearFreeBufferIndexes()");
    _voxelsInWriteArrays = 0; // reset our VBO
    _abandonedVBOSlots = 0;

    // clear out freeIndexes
    {
        PerformanceWarning warn(showWarnings,"clearFreeBufferIndexes() : _freeIndexLock.lock()");
        _freeIndexLock.lock();
    }
    {
        PerformanceWarning warn(showWarnings,"clearFreeBufferIndexes() : _freeIndexes.clear()");
        _freeIndexes.clear();
    }
    _freeIndexLock.unlock();
}

VoxelSystem::~VoxelSystem() {
    VoxelTreeElement::removeDeleteHook(this);
    VoxelTreeElement::removeUpdateHook(this);

    cleanupVoxelMemory();
    delete _tree;
}


// This is called by the main application thread on both the initialization of the application and when
// the preferences dialog box is called/saved
void VoxelSystem::setMaxVoxels(unsigned long maxVoxels) {
    if (maxVoxels == _maxVoxels) {
        return;
    }
    bool wasInitialized = _initialized;
    if (wasInitialized) {
        clearAllNodesBufferIndex();
        cleanupVoxelMemory();
    }
    _maxVoxels = maxVoxels;
    if (wasInitialized) {
        initVoxelMemory();
    }
    if (wasInitialized) {
        forceRedrawEntireTree();
    }
}

// This is called by the main application thread on both the initialization of the application and when
// the use voxel shader menu item is chosen
void VoxelSystem::setUseVoxelShader(bool useVoxelShader) {
    if (_useVoxelShader == useVoxelShader) {
        return;
    }

    bool wasInitialized = _initialized;
    if (wasInitialized) {
        clearAllNodesBufferIndex();
        cleanupVoxelMemory();
    }
    _useVoxelShader = useVoxelShader;
    _usePrimitiveRenderer = false;
    if (wasInitialized) {
        initVoxelMemory();
    }

    if (wasInitialized) {
        forceRedrawEntireTree();
    }
}

void VoxelSystem::setVoxelsAsPoints(bool voxelsAsPoints) {
    if (_voxelsAsPoints == voxelsAsPoints) {
        return;
    }

    bool wasInitialized = _initialized;

    // If we're "turning on" Voxels as points, we need to double check that we're in voxel shader mode.
    // Voxels as points uses the VoxelShader memory model, so if we're not in voxel shader mode,
    // then set it to voxel shader mode.
    if (voxelsAsPoints) {
        Menu::getInstance()->getUseVoxelShader()->setEnabled(false);

        // If enabling this... then do it before checking voxel shader status, that way, if voxel
        // shader is already enabled, we just start drawing as points.
        _voxelsAsPoints = true;

        if (!_useVoxelShader) {
            setUseVoxelShader(true);
            _voxelShaderModeWhenVoxelsAsPointsEnabled = false;
        } else {
            _voxelShaderModeWhenVoxelsAsPointsEnabled = true;
        }
    } else {
        Menu::getInstance()->getUseVoxelShader()->setEnabled(true);
        // if we're turning OFF voxels as point mode, then we check what the state of voxel shader was when we enabled
        // voxels as points, if it was OFF, then we return it to that value.
        if (_voxelShaderModeWhenVoxelsAsPointsEnabled == false) {
            setUseVoxelShader(false);
        }
        // If disabling this... then do it AFTER checking previous voxel shader status, that way, if voxel
        // shader is was not enabled, we switch back to normal mode before turning off points.
        _voxelsAsPoints = false;
    }

    // Set our voxels as points
    if (wasInitialized) {
        forceRedrawEntireTree();
    }
}

void VoxelSystem::cleanupVoxelMemory() {
    if (_initialized) {
        _readArraysLock.lockForWrite();
        _initialized = false; // no longer initialized
        if (_useVoxelShader) {
            // these are used when in VoxelShader mode.
            glDeleteBuffers(1, &_vboVoxelsID);
            glDeleteBuffers(1, &_vboVoxelsIndicesID);

            delete[] _writeVoxelShaderData;
            delete[] _readVoxelShaderData;

            _writeVoxelShaderData = _readVoxelShaderData = NULL;

        } else {
            // Destroy  glBuffers
            glDeleteBuffers(1, &_vboVerticesID);
            glDeleteBuffers(1, &_vboColorsID);

            glDeleteBuffers(1, &_vboIndicesTop);
            glDeleteBuffers(1, &_vboIndicesBottom);
            glDeleteBuffers(1, &_vboIndicesLeft);
            glDeleteBuffers(1, &_vboIndicesRight);
            glDeleteBuffers(1, &_vboIndicesFront);
            glDeleteBuffers(1, &_vboIndicesBack);

            delete[] _readVerticesArray;
            delete[] _writeVerticesArray;
            delete[] _readColorsArray;
            delete[] _writeColorsArray;

            _readVerticesArray = NULL;
            _writeVerticesArray = NULL;
            _readColorsArray = NULL;
            _writeColorsArray = NULL;
        }

        delete _renderer;
        _renderer = 0;

        delete[] _writeVoxelDirtyArray;
        delete[] _readVoxelDirtyArray;
        _writeVoxelDirtyArray = _readVoxelDirtyArray = NULL;
        _readArraysLock.unlock();
    
    }
}

void VoxelSystem::setupFaceIndices(GLuint& faceVBOID, GLubyte faceIdentityIndices[]) {
    GLuint* indicesArray = new GLuint[INDICES_PER_FACE * _maxVoxels];

    // populate the indicesArray
    // this will not change given new voxels, so we can set it all up now
    for (unsigned long n = 0; n < _maxVoxels; n++) {
        // fill the indices array
        int voxelIndexOffset = n * INDICES_PER_FACE;
        GLuint* currentIndicesPos = indicesArray + voxelIndexOffset;
        int startIndex = (n * GLOBAL_NORMALS_VERTICES_PER_VOXEL);

        for (int i = 0; i < INDICES_PER_FACE; i++) {
            // add indices for this side of the cube
            currentIndicesPos[i] = startIndex + faceIdentityIndices[i];
        }
    }

    glGenBuffers(1, &faceVBOID);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, faceVBOID);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 INDICES_PER_FACE * sizeof(GLuint) * _maxVoxels,
                 indicesArray, GL_STATIC_DRAW);
    _memoryUsageVBO += INDICES_PER_FACE * sizeof(GLuint) * _maxVoxels;

    // delete the indices and normals arrays that are no longer needed
    delete[] indicesArray;
}

void VoxelSystem::initVoxelMemory() {
    _readArraysLock.lockForWrite();
    _writeArraysLock.lockForWrite();

    _memoryUsageRAM = 0;
    _memoryUsageVBO = 0; // our VBO allocations as we know them

    // if _voxelsAsPoints then we must have _useVoxelShader
    if (_voxelsAsPoints && !_useVoxelShader) {
        _useVoxelShader = true;
    }

    if (_useVoxelShader) {
        
        // voxel instance shader
        _voxelInstanceProgram.addShaderFromSourceFile(QGLShader::Vertex, Application::resourcesPath() + "shaders/voxel_instance.vert");
        _voxelInstanceProgram.addShaderFromSourceFile(QGLShader::Fragment, Application::resourcesPath() + "shaders/voxel_instance.frag");
        _voxelInstanceProgram.link();
        
        _voxelInstanceProgram.bind();

        // shader attributes
        _translationShaderAttributeLocation = _voxelInstanceProgram.attributeLocation("translation");
        _scaleShaderAttributeLocation = _voxelInstanceProgram.attributeLocation("scale");
        _colorShaderAttributeLocation = _voxelInstanceProgram.attributeLocation("color");
        _positionShaderAttributeLocation = _voxelInstanceProgram.attributeLocation("position");
        _normalShaderAttributeLocation = _voxelInstanceProgram.attributeLocation("normal");
        _uvShaderAttributeLocation = _voxelInstanceProgram.attributeLocation("uv");
        
        createCube();
        
        // voxel info data
        int voxelInfoSize = _maxVoxels * sizeof(VoxelInstanceShaderVBOData);
        _outputInstanceVoxelData = new VoxelInstanceShaderVBOData[_maxVoxels];
        memset(_outputInstanceVoxelData, 0, voxelInfoSize);

        _voxelInstanceProgram.release();
        
        size_t strideSize = sizeof(GLfloat) * 4;
        
        // voxel info buffer
        glGenBuffers(1, &_voxelInfoID);
        glBindBuffer(GL_ARRAY_BUFFER, _voxelInfoID);
        glBufferData(GL_ARRAY_BUFFER, sizeof(VoxelInstanceShaderVBOData) * _maxVoxels, _outputInstanceVoxelData, GL_DYNAMIC_DRAW);
        
        // translation
        glVertexAttribPointer(_translationShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, sizeof(VoxelInstanceShaderVBOData), 0);
        glEnableVertexAttribArray(_translationShaderAttributeLocation);
        glVertexAttribDivisorARB(_translationShaderAttributeLocation, 1);
        
        // scale
        glVertexAttribPointer(_scaleShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, sizeof(VoxelInstanceShaderVBOData), (void *)strideSize);
        glEnableVertexAttribArray(_scaleShaderAttributeLocation);
        glVertexAttribDivisorARB(_scaleShaderAttributeLocation, 1);
        
        // color
        glVertexAttribPointer(_colorShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, sizeof(VoxelInstanceShaderVBOData), (void *)(strideSize * 2));
        glEnableVertexAttribArray(_colorShaderAttributeLocation);
        glVertexAttribDivisorARB(_colorShaderAttributeLocation, 1);
        
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        
        _memoryUsageVBO += voxelInfoSize;
        _memoryUsageRAM += voxelInfoSize;
    
#if 0
        GLuint* indicesArray = new GLuint[_maxVoxels];

        // populate the indicesArray
        // this will not change given new voxels, so we can set it all up now
        for (unsigned long n = 0; n < _maxVoxels; n++) {
            indicesArray[n] = n;
        }

        // bind the indices VBO to the actual indices array
        glGenBuffers(1, &_vboVoxelsIndicesID);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _vboVoxelsIndicesID);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLuint) * _maxVoxels, indicesArray, GL_STATIC_DRAW);
        _memoryUsageVBO += sizeof(GLuint) * _maxVoxels;

        glGenBuffers(1, &_vboVoxelsID);
        glBindBuffer(GL_ARRAY_BUFFER, _vboVoxelsID);
        glBufferData(GL_ARRAY_BUFFER, _maxVoxels * sizeof(VoxelShaderVBOData), NULL, GL_DYNAMIC_DRAW);
        _memoryUsageVBO += _maxVoxels * sizeof(VoxelShaderVBOData);

        // delete the indices and normals arrays that are no longer needed
        delete[] indicesArray;
#endif // #if 0
        
        // prep the data structures for incoming voxel data
        _writeVoxelShaderData = new VoxelShaderVBOData[_maxVoxels];
        _memoryUsageRAM += (sizeof(VoxelShaderVBOData) * _maxVoxels);
        
        _readVoxelShaderData = new VoxelShaderVBOData[_maxVoxels];
        _memoryUsageRAM += (sizeof(VoxelShaderVBOData) * _maxVoxels);

        // we will track individual dirty sections with these arrays of bools
        _writeVoxelDirtyArray = new bool[_maxVoxels];
        memset(_writeVoxelDirtyArray, false, _maxVoxels * sizeof(bool));
        _memoryUsageRAM += (_maxVoxels * sizeof(bool));

        _readVoxelDirtyArray = new bool[_maxVoxels];
        memset(_readVoxelDirtyArray, false, _maxVoxels * sizeof(bool));
        _memoryUsageRAM += (_maxVoxels * sizeof(bool));
   
    } else {

        // Global Normals mode uses a technique of not including normals on any voxel vertices, and instead
        // rendering the voxel faces in 6 passes that use a global call to glNormal3f()
        setupFaceIndices(_vboIndicesTop,    identityIndicesTop);
        setupFaceIndices(_vboIndicesBottom, identityIndicesBottom);
        setupFaceIndices(_vboIndicesLeft,   identityIndicesLeft);
        setupFaceIndices(_vboIndicesRight,  identityIndicesRight);
        setupFaceIndices(_vboIndicesFront,  identityIndicesFront);
        setupFaceIndices(_vboIndicesBack,   identityIndicesBack);

        // Depending on if we're using per vertex normals, we will need more or less vertex points per voxel
        int vertexPointsPerVoxel = GLOBAL_NORMALS_VERTEX_POINTS_PER_VOXEL;
        glGenBuffers(1, &_vboVerticesID);
        glBindBuffer(GL_ARRAY_BUFFER, _vboVerticesID);
        glBufferData(GL_ARRAY_BUFFER, vertexPointsPerVoxel * sizeof(GLfloat) * _maxVoxels, NULL, GL_DYNAMIC_DRAW);
        _memoryUsageVBO += vertexPointsPerVoxel * sizeof(GLfloat) * _maxVoxels;

        // VBO for colorsArray
        glGenBuffers(1, &_vboColorsID);
        glBindBuffer(GL_ARRAY_BUFFER, _vboColorsID);
        glBufferData(GL_ARRAY_BUFFER, vertexPointsPerVoxel * sizeof(GLubyte) * _maxVoxels, NULL, GL_DYNAMIC_DRAW);
        _memoryUsageVBO += vertexPointsPerVoxel * sizeof(GLubyte) * _maxVoxels;

        // we will track individual dirty sections with these arrays of bools
        _writeVoxelDirtyArray = new bool[_maxVoxels];
        memset(_writeVoxelDirtyArray, false, _maxVoxels * sizeof(bool));
        _memoryUsageRAM += (sizeof(bool) * _maxVoxels);

        _readVoxelDirtyArray = new bool[_maxVoxels];
        memset(_readVoxelDirtyArray, false, _maxVoxels * sizeof(bool));
        _memoryUsageRAM += (sizeof(bool) * _maxVoxels);

        // prep the data structures for incoming voxel data
        _writeVerticesArray = new GLfloat[vertexPointsPerVoxel * _maxVoxels];
        _memoryUsageRAM += (sizeof(GLfloat) * vertexPointsPerVoxel * _maxVoxels);
        _readVerticesArray = new GLfloat[vertexPointsPerVoxel * _maxVoxels];
        _memoryUsageRAM += (sizeof(GLfloat) * vertexPointsPerVoxel * _maxVoxels);

        _writeColorsArray = new GLubyte[vertexPointsPerVoxel * _maxVoxels];
        _memoryUsageRAM += (sizeof(GLubyte) * vertexPointsPerVoxel * _maxVoxels);
        _readColorsArray = new GLubyte[vertexPointsPerVoxel * _maxVoxels];
        _memoryUsageRAM += (sizeof(GLubyte) * vertexPointsPerVoxel * _maxVoxels);

        // create our simple fragment shader if we're the first system to init
        if (!_shadowMapProgram.isLinked()) {
            _shadowMapProgram.addShaderFromSourceFile(QGLShader::Vertex,
                Application::resourcesPath() + "shaders/shadow_map.vert");
            _shadowMapProgram.addShaderFromSourceFile(QGLShader::Fragment,
                Application::resourcesPath() + "shaders/shadow_map.frag");
            _shadowMapProgram.link();

            _shadowMapProgram.bind();
            _shadowMapProgram.setUniformValue("shadowMap", 0);
            _shadowMapProgram.release();
            
            _cascadedShadowMapProgram.addShaderFromSourceFile(QGLShader::Vertex,
                Application::resourcesPath() + "shaders/cascaded_shadow_map.vert");
            _cascadedShadowMapProgram.addShaderFromSourceFile(QGLShader::Fragment,
                Application::resourcesPath() + "shaders/cascaded_shadow_map.frag");
            _cascadedShadowMapProgram.link();

            _cascadedShadowMapProgram.bind();
            _cascadedShadowMapProgram.setUniformValue("shadowMap", 0);
            _shadowDistancesLocation = _cascadedShadowMapProgram.uniformLocation("shadowDistances");
            _cascadedShadowMapProgram.release();
        }
        
    }
    _renderer = new PrimitiveRenderer(_maxVoxels);

    _initialized = true;
    
    _writeArraysLock.unlock();
    _readArraysLock.unlock();
    
    // fog for haze
    if (_drawHaze) {
        GLfloat fogColor[] = {_hazeColor.x, _hazeColor.y, _hazeColor.z, 1.0f};
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogfv(GL_FOG_COLOR, fogColor);
        glFogf(GL_FOG_START, 0.0f);
        glFogf(GL_FOG_END, _farHazeDistance);
    }
}

int VoxelSystem::parseData(const QByteArray& packet) {
    bool showTimingDetails = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showTimingDetails, "VoxelSystem::parseData()",showTimingDetails);

    PacketType command = packetTypeForPacket(packet);
    int numBytesPacketHeader = numBytesForPacketHeader(packet);
    switch(command) {
        case PacketTypeVoxelData: {
            PerformanceWarning warn(showTimingDetails, "VoxelSystem::parseData() PacketType_VOXEL_DATA part...",showTimingDetails);
            
            const unsigned char* dataAt = reinterpret_cast<const unsigned char*>(packet.data()) + numBytesPacketHeader;

            OCTREE_PACKET_FLAGS flags = (*(OCTREE_PACKET_FLAGS*)(dataAt));
            dataAt += sizeof(OCTREE_PACKET_FLAGS);
            OCTREE_PACKET_SEQUENCE sequence = (*(OCTREE_PACKET_SEQUENCE*)dataAt);
            dataAt += sizeof(OCTREE_PACKET_SEQUENCE);

            OCTREE_PACKET_SENT_TIME sentAt = (*(OCTREE_PACKET_SENT_TIME*)dataAt);
            dataAt += sizeof(OCTREE_PACKET_SENT_TIME);

            bool packetIsColored = oneAtBit(flags, PACKET_IS_COLOR_BIT);
            bool packetIsCompressed = oneAtBit(flags, PACKET_IS_COMPRESSED_BIT);

            OCTREE_PACKET_SENT_TIME arrivedAt = usecTimestampNow();
            int flightTime = arrivedAt - sentAt;

            OCTREE_PACKET_INTERNAL_SECTION_SIZE sectionLength = 0;
            unsigned int dataBytes = packet.size() - (numBytesPacketHeader + OCTREE_PACKET_EXTRA_HEADERS_SIZE);

            int subsection = 1;
            while (dataBytes > 0) {
                if (packetIsCompressed) {
                    if (dataBytes > sizeof(OCTREE_PACKET_INTERNAL_SECTION_SIZE)) {
                        sectionLength = (*(OCTREE_PACKET_INTERNAL_SECTION_SIZE*)dataAt);
                        dataAt += sizeof(OCTREE_PACKET_INTERNAL_SECTION_SIZE);
                        dataBytes -= sizeof(OCTREE_PACKET_INTERNAL_SECTION_SIZE);
                    } else {
                        sectionLength = 0;
                        dataBytes = 0; // stop looping something is wrong
                    }
                } else {
                    sectionLength = dataBytes;
                }

                if (sectionLength) {
                    PerformanceWarning warn(showTimingDetails, "VoxelSystem::parseData() section");
                    // ask the VoxelTree to read the bitstream into the tree
                    ReadBitstreamToTreeParams args(packetIsColored ? WANT_COLOR : NO_COLOR, WANT_EXISTS_BITS, NULL, getDataSourceUUID());
                    _tree->lockForWrite();
                    OctreePacketData packetData(packetIsCompressed);
                    packetData.loadFinalizedContent(dataAt, sectionLength);
                    if (Application::getInstance()->getLogger()->extraDebugging()) {
                        qDebug("VoxelSystem::parseData() ... Got Packet Section"
                               " color:%s compressed:%s sequence: %u flight:%d usec size:%d data:%u"
                               " subsection:%d sectionLength:%d uncompressed:%d",
                            debug::valueOf(packetIsColored), debug::valueOf(packetIsCompressed),
                            sequence, flightTime, packet.size(), dataBytes, subsection, sectionLength,
                               packetData.getUncompressedSize());
                    }
                    _tree->readBitstreamToTree(packetData.getUncompressedData(), packetData.getUncompressedSize(), args);
                    _tree->unlock();

                    dataBytes -= sectionLength;
                    dataAt += sectionLength;
                }
            }
            subsection++;
        }
        default:
            break;
    }
    if (!_useFastVoxelPipeline || _writeRenderFullVBO) {
        setupNewVoxelsForDrawing();
    } else {
        setupNewVoxelsForDrawingSingleNode(DONT_BAIL_EARLY);
    }

    Application::getInstance()->getBandwidthMeter()->inputStream(BandwidthMeter::VOXELS).updateValue(packet.size());

    return packet.size();
}

void VoxelSystem::setupNewVoxelsForDrawing() {
    PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings),
                            "setupNewVoxelsForDrawing()");

    if (!_initialized) {
        return; // bail early if we're not initialized
    }

    quint64 start = usecTimestampNow();
    quint64 sinceLastTime = (start - _setupNewVoxelsForDrawingLastFinished) / 1000;

    bool iAmDebugging = false;  // if you're debugging set this to true, so you won't get skipped for slow debugging
    if (!iAmDebugging && sinceLastTime <= std::max((float) _setupNewVoxelsForDrawingLastElapsed, SIXTY_FPS_IN_MILLISECONDS)) {
        return; // bail early, it hasn't been long enough since the last time we ran
    }

    _inSetupNewVoxelsForDrawing = true;
    
    bool didWriteFullVBO = _writeRenderFullVBO;
    if (_tree->isDirty()) {
        static char buffer[64] = { 0 };
        if (Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings)) {
            sprintf(buffer, "newTreeToArrays() _writeRenderFullVBO=%s", debug::valueOf(_writeRenderFullVBO));
        };
        PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings), buffer);
        _callsToTreesToArrays++;

        if (_writeRenderFullVBO) {
            if (_usePrimitiveRenderer) {
                _renderer->release();
                clearAllNodesBufferIndex();
            }
            clearFreeBufferIndexes();
        }
        _voxelsUpdated = newTreeToArrays(_tree->getRoot());
        _tree->clearDirtyBit(); // after we pull the trees into the array, we can consider the tree clean

        if (_writeRenderFullVBO) {
            _abandonedVBOSlots = 0; // reset the count of our abandoned slots, why is this here and not earlier????
        }

        _writeRenderFullVBO = false;
    } else {
        _voxelsUpdated = 0;
    }

    if (_usePrimitiveRenderer) {
        if (_voxelsUpdated) {
            _voxelsDirty=true;
        }
    } else {
        // lock on the buffer write lock so we can't modify the data when the GPU is reading it
        _readArraysLock.lockForWrite();

        if (_voxelsUpdated) {
            _voxelsDirty=true;
        }

        // copy the newly written data to the arrays designated for reading, only does something if _voxelsDirty && _voxelsUpdated
        copyWrittenDataToReadArrays(didWriteFullVBO);
        _readArraysLock.unlock();

    }

    quint64 end = usecTimestampNow();
    int elapsedmsec = (end - start) / 1000;
    _setupNewVoxelsForDrawingLastFinished = end;
    _setupNewVoxelsForDrawingLastElapsed = elapsedmsec;
    _inSetupNewVoxelsForDrawing = false;

    bool extraDebugging = Application::getInstance()->getLogger()->extraDebugging();
    if (extraDebugging) {
        qDebug("setupNewVoxelsForDrawing()... _voxelsUpdated=%lu...",_voxelsUpdated);
        _viewFrustum->printDebugDetails();
    }
}

void VoxelSystem::setupNewVoxelsForDrawingSingleNode(bool allowBailEarly) {
    PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings),
                            "setupNewVoxelsForDrawingSingleNode() xxxxx");

    quint64 start = usecTimestampNow();
    quint64 sinceLastTime = (start - _setupNewVoxelsForDrawingLastFinished) / 1000;

    bool iAmDebugging = false;  // if you're debugging set this to true, so you won't get skipped for slow debugging
    if (allowBailEarly && !iAmDebugging &&
        sinceLastTime <= std::max((float) _setupNewVoxelsForDrawingLastElapsed, SIXTY_FPS_IN_MILLISECONDS)) {
        return; // bail early, it hasn't been long enough since the last time we ran
    }

    if (_usePrimitiveRenderer) {
        _voxelsDirty = true; // if we got this far, then we can assume some voxels are dirty
        _voxelsUpdated = 0;
    } else {
        // lock on the buffer write lock so we can't modify the data when the GPU is reading it
        {
            PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings),
                                    "setupNewVoxelsForDrawingSingleNode()... _bufferWriteLock.lock();" );
            _readArraysLock.lockForWrite();
        }

        _voxelsDirty = true; // if we got this far, then we can assume some voxels are dirty

        // copy the newly written data to the arrays designated for reading, only does something if _voxelsDirty && _voxelsUpdated
        copyWrittenDataToReadArrays(_writeRenderFullVBO);

        // after...
        _voxelsUpdated = 0;
        _readArraysLock.unlock();
    }
    quint64 end = usecTimestampNow();
    int elapsedmsec = (end - start) / 1000;
    _setupNewVoxelsForDrawingLastFinished = end;
    _setupNewVoxelsForDrawingLastElapsed = elapsedmsec;
}



class recreateVoxelGeometryInViewArgs {
public:
    VoxelSystem* thisVoxelSystem;
    ViewFrustum thisViewFrustum;
    unsigned long nodesScanned;
    float voxelSizeScale;
    int boundaryLevelAdjust;

    recreateVoxelGeometryInViewArgs(VoxelSystem* voxelSystem) :
        thisVoxelSystem(voxelSystem),
        thisViewFrustum(*voxelSystem->getViewFrustum()),
        nodesScanned(0),
        voxelSizeScale(Menu::getInstance()->getVoxelSizeScale()),
        boundaryLevelAdjust(Menu::getInstance()->getBoundaryLevelAdjust())
    {
    }
};

// The goal of this operation is to remove any old references to old geometry, and if the voxel
// should be visible, create new geometry for it.
bool VoxelSystem::recreateVoxelGeometryInViewOperation(OctreeElement* element, void* extraData) {
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;
    recreateVoxelGeometryInViewArgs* args = (recreateVoxelGeometryInViewArgs*)extraData;

    args->nodesScanned++;
    
    // reset the old geometry...
    // note: this doesn't "mark the voxel as changed", so it only releases the old buffer index thereby forgetting the
    // old geometry
    voxel->setBufferIndex(GLBUFFER_INDEX_UNKNOWN); 

    bool shouldRender = voxel->calculateShouldRender(&args->thisViewFrustum, args->voxelSizeScale, args->boundaryLevelAdjust);
    bool inView = voxel->isInView(args->thisViewFrustum);
    voxel->setShouldRender(inView && shouldRender);
    if (shouldRender && inView) {
        // recreate the geometry
        args->thisVoxelSystem->updateNodeInArrays(voxel, false, true); // DONT_REUSE_INDEX, FORCE_REDRAW
    }

    return true; // keep recursing!
}


// TODO: does cleanupRemovedVoxels() ever get called?
// TODO: other than cleanupRemovedVoxels() is there anyplace we attempt to detect too many abandoned slots???
void VoxelSystem::recreateVoxelGeometryInView() {

    qDebug() << "recreateVoxelGeometryInView()...";

    recreateVoxelGeometryInViewArgs args(this);
    _writeArraysLock.lockForWrite(); // don't let anyone read or write our write arrays until we're done
    _tree->lockForRead(); // don't let anyone change our tree structure until we're run
    
    // reset our write arrays bookkeeping to think we've got no voxels in it
    clearFreeBufferIndexes();

    // do we need to reset out _writeVoxelDirtyArray arrays??
    memset(_writeVoxelDirtyArray, false, _maxVoxels * sizeof(bool));
    
    _tree->recurseTreeWithOperation(recreateVoxelGeometryInViewOperation,(void*)&args);
    _tree->unlock();
    _writeArraysLock.unlock();
}

void VoxelSystem::checkForCulling() {
    PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings), "checkForCulling()");
    quint64 start = usecTimestampNow();

    // track how long its been since we were last moving. If we have recently moved then only use delta frustums, if
    // it's been a long time since we last moved, then go ahead and do a full frustum cull.
    if (isViewChanging()) {
        _lastViewIsChanging = start;
    }
    quint64 sinceLastMoving = (start - _lastViewIsChanging) / 1000;
    bool enoughTime = (sinceLastMoving >= std::max((float) _lastViewCullingElapsed, VIEW_CULLING_RATE_IN_MILLISECONDS));

    // These has changed events will occur before we stop. So we need to remember this for when we finally have stopped
    // moving long enough to be enoughTime
    if (hasViewChanged()) {
        _hasRecentlyChanged = true;
    }

    // If we have recently changed, but it's been enough time since we last moved, then we will do a full frustum
    // hide/show culling pass
    bool forceFullFrustum = enoughTime && _hasRecentlyChanged;

    // in hide mode, we only track the full frustum culls, because we don't care about the partials.
    if (forceFullFrustum) {
        _lastViewCulling = start;
        _hasRecentlyChanged = false;
    }

    // This would be a good place to do a special processing pass, for example, switching the LOD of the scene
    bool fullRedraw = (_lastKnownVoxelSizeScale != Menu::getInstance()->getVoxelSizeScale() || 
                        _lastKnownBoundaryLevelAdjust != Menu::getInstance()->getBoundaryLevelAdjust());

    _lastKnownVoxelSizeScale = Menu::getInstance()->getVoxelSizeScale();
    _lastKnownBoundaryLevelAdjust = Menu::getInstance()->getBoundaryLevelAdjust();

    if (fullRedraw) {
        // this will remove all old geometry and recreate the correct geometry for all in view voxels
        recreateVoxelGeometryInView();
    } else {
        hideOutOfView(forceFullFrustum);
    }

    if (forceFullFrustum) {
        quint64 endViewCulling = usecTimestampNow();
        _lastViewCullingElapsed = (endViewCulling - start) / 1000;
    }

    // Once we call cleanupRemovedVoxels() we do need to rebuild our VBOs (if anything was actually removed). So,
    // we should consider putting this someplace else... as this might be able to occur less frequently, and save us on
    // VBO reubuilding. Possibly we should do this only if our actual VBO usage crosses some lower boundary.
    cleanupRemovedVoxels();
}

void VoxelSystem::cleanupRemovedVoxels() {
    PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings), "cleanupRemovedVoxels()");
    // This handles cleanup of voxels that were culled as part of our regular out of view culling operation
    if (!_removedVoxels.isEmpty()) {
        if (Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings)) {
            qDebug() << "cleanupRemovedVoxels().. _removedVoxels=" << _removedVoxels.count();
        }
        while (!_removedVoxels.isEmpty()) {
            delete _removedVoxels.extract();
        }
        _writeRenderFullVBO = true; // if we remove voxels, we must update our full VBOs
    }

    // we also might have VBO slots that have been abandoned, if too many of our VBO slots
    // are abandonded we want to rerender our full VBOs
    const float TOO_MANY_ABANDONED_RATIO = 0.5f;
    if (!_usePrimitiveRenderer && !_writeRenderFullVBO && 
        (_abandonedVBOSlots > (_voxelsInWriteArrays * TOO_MANY_ABANDONED_RATIO))) {
        if (Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings)) {
            qDebug() << "cleanupRemovedVoxels().. _abandonedVBOSlots ["
                << _abandonedVBOSlots << "] > TOO_MANY_ABANDONED_RATIO";
        }
        _writeRenderFullVBO = true;
    }
}

void VoxelSystem::copyWrittenDataToReadArraysFullVBOs() {
    copyWrittenDataSegmentToReadArrays(0, _voxelsInWriteArrays - 1);
    _voxelsInReadArrays = _voxelsInWriteArrays;

    // clear our dirty flags
    memset(_writeVoxelDirtyArray, false, _voxelsInWriteArrays * sizeof(bool));

    // let the reader know to get the full array
    _readRenderFullVBO = true;
}

void VoxelSystem::copyWrittenDataToReadArraysPartialVBOs() {
    glBufferIndex segmentStart = 0;
    bool inSegment = false;
    for (glBufferIndex i = 0; i < _voxelsInWriteArrays; i++) {
        bool thisVoxelDirty = _writeVoxelDirtyArray[i];
        _readVoxelDirtyArray[i] |= thisVoxelDirty;
        _writeVoxelDirtyArray[i] = false;
        if (!inSegment) {
            if (thisVoxelDirty) {
                segmentStart = i;
                inSegment = true;
            }
        } else {
            if (!thisVoxelDirty) {
                // If we got here because because this voxel is NOT dirty, so the last dirty voxel was the one before
                // this one and so that's where the "segment" ends
                copyWrittenDataSegmentToReadArrays(segmentStart, i - 1);
                inSegment = false;
            }
        }
    }

    // if we got to the end of the array, and we're in an active dirty segment...
    if (inSegment) {
        copyWrittenDataSegmentToReadArrays(segmentStart, _voxelsInWriteArrays - 1);
    }

    // update our length
    _voxelsInReadArrays = _voxelsInWriteArrays;
}

void VoxelSystem::copyWrittenDataSegmentToReadArrays(glBufferIndex segmentStart, glBufferIndex segmentEnd) {
    int segmentLength = (segmentEnd - segmentStart) + 1;
    if (_useVoxelShader) {
        GLsizeiptr segmentSizeBytes = segmentLength * sizeof(VoxelShaderVBOData);
        void* readDataAt = &_readVoxelShaderData[segmentStart];
        void* writeDataAt = &_writeVoxelShaderData[segmentStart];
        memcpy(readDataAt, writeDataAt, segmentSizeBytes);
        
        float oneOverTwoFiftyFive = 1.0f / 255.0f;
        float coordScaling = 2.0f; // _treeScale * 0.0001f;
        
        for (int i = segmentStart; i <= segmentEnd; i++) {
            // position
            _outputInstanceVoxelData[i].x = _writeVoxelShaderData[i].x * coordScaling;
            _outputInstanceVoxelData[i].y = _writeVoxelShaderData[i].y * coordScaling;
            _outputInstanceVoxelData[i].z = _writeVoxelShaderData[i].z * coordScaling;
            _outputInstanceVoxelData[i].w = 1.0f;
            
            // scale
            float scale = _writeVoxelShaderData[i].s;
            _outputInstanceVoxelData[i].sx = scale * coordScaling;
            _outputInstanceVoxelData[i].sy = scale * coordScaling;
            _outputInstanceVoxelData[i].sz = scale * coordScaling;
            _outputInstanceVoxelData[i].sw = 1.0f;
            
            // color
            _outputInstanceVoxelData[i].red = (float)_writeVoxelShaderData[i].r * oneOverTwoFiftyFive;
            _outputInstanceVoxelData[i].green = (float)_writeVoxelShaderData[i].g * oneOverTwoFiftyFive;
            _outputInstanceVoxelData[i].blue = (float)_writeVoxelShaderData[i].b * oneOverTwoFiftyFive;
            _outputInstanceVoxelData[i].alpha = 1.0f;
        }
        
    } else {
        // Depending on if we're using per vertex normals, we will need more or less vertex points per voxel
        int vertexPointsPerVoxel = GLOBAL_NORMALS_VERTEX_POINTS_PER_VOXEL;

        GLsizeiptr segmentSizeBytes = segmentLength * vertexPointsPerVoxel * sizeof(GLfloat);
        GLfloat* readVerticesAt     = _readVerticesArray  + (segmentStart * vertexPointsPerVoxel);
        GLfloat* writeVerticesAt    = _writeVerticesArray + (segmentStart * vertexPointsPerVoxel);
        memcpy(readVerticesAt, writeVerticesAt, segmentSizeBytes);

        segmentSizeBytes        = segmentLength * vertexPointsPerVoxel * sizeof(GLubyte);
        GLubyte* readColorsAt   = _readColorsArray   + (segmentStart * vertexPointsPerVoxel);
        GLubyte* writeColorsAt  = _writeColorsArray  + (segmentStart * vertexPointsPerVoxel);
        memcpy(readColorsAt, writeColorsAt, segmentSizeBytes);
    }
}

void VoxelSystem::copyWrittenDataToReadArrays(bool fullVBOs) {
    static unsigned int lockForReadAttempt = 0;
    static unsigned int lockForWriteAttempt = 0;
    PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings),
                            "copyWrittenDataToReadArrays()");

    // attempt to get the writeArraysLock for reading and the readArraysLock for writing 
    // so we can copy from the write to the read...  if we fail, that's ok, we'll get it the next
    // time around, the only side effect is the VBOs won't be updated this frame
    const int WAIT_FOR_LOCK_IN_MS = 5;
    if (_readArraysLock.tryLockForWrite(WAIT_FOR_LOCK_IN_MS)) {
        lockForWriteAttempt = 0;
        if (_writeArraysLock.tryLockForRead(WAIT_FOR_LOCK_IN_MS)) {
            lockForReadAttempt = 0;
            if (_voxelsDirty && _voxelsUpdated) {
                if (fullVBOs) {
                    copyWrittenDataToReadArraysFullVBOs();
                } else {
                    copyWrittenDataToReadArraysPartialVBOs();
                }
            }
            _writeArraysLock.unlock();
        } else {
            lockForReadAttempt++;
            // only report error of first failure
            if (lockForReadAttempt == 1) {
                qDebug() << "couldn't get _writeArraysLock.LockForRead()...";
            }
        }
        _readArraysLock.unlock();
    } else {
        lockForWriteAttempt++;
        // only report error of first failure
        if (lockForWriteAttempt == 1) {
            qDebug() << "couldn't get _readArraysLock.LockForWrite()...";
        }
    }
}

int VoxelSystem::newTreeToArrays(VoxelTreeElement* voxel) {
    int   voxelsUpdated   = 0;
    bool  shouldRender    = false; // assume we don't need to render it
    // if it's colored, we might need to render it!
    float voxelSizeScale = Menu::getInstance()->getVoxelSizeScale();;
    int boundaryLevelAdjust = Menu::getInstance()->getBoundaryLevelAdjust();
    shouldRender = voxel->calculateShouldRender(_viewFrustum, voxelSizeScale, boundaryLevelAdjust);

    voxel->setShouldRender(shouldRender);
    // let children figure out their renderness
    if (!voxel->isLeaf()) {

        // As we check our children, see if any of them went from shouldRender to NOT shouldRender
        // then we probably dropped LOD and if we don't have color, we want to average our children
        // for a new color.
        int childrenGotHiddenCount = 0;
        for (int i = 0; i < NUMBER_OF_CHILDREN; i++) {
            VoxelTreeElement* childVoxel = voxel->getChildAtIndex(i);
            if (childVoxel) {
                bool wasShouldRender = childVoxel->getShouldRender();
                voxelsUpdated += newTreeToArrays(childVoxel);
                bool isShouldRender = childVoxel->getShouldRender();
                if (wasShouldRender && !isShouldRender) {
                    childrenGotHiddenCount++;
                }
            }
        }
        if (childrenGotHiddenCount > 0) {
            voxel->calculateAverageFromChildren();
        }
    }

    // update their geometry in the array. depending on our over all mode (fullVBO or not) we will reuse or not reuse the index
    if (_writeRenderFullVBO) {
        const bool DONT_REUSE_INDEX = false;
        const bool FORCE_REDRAW = true;
        voxelsUpdated += updateNodeInArrays(voxel, DONT_REUSE_INDEX, FORCE_REDRAW);
    } else {
        const bool REUSE_INDEX = true;
        const bool DONT_FORCE_REDRAW = false;
        voxelsUpdated += updateNodeInArrays(voxel, REUSE_INDEX, DONT_FORCE_REDRAW);
    }
    voxel->clearDirtyBit(); // clear the dirty bit, do this before we potentially delete things.

    return voxelsUpdated;
}

// called as response to elementDeleted() in fast pipeline case. The node
// is being deleted, but it's state is such that it thinks it should render
// and therefore we can't use the normal render calculations. This method
// will forcibly remove it from the VBOs because we know better!!!
int VoxelSystem::forceRemoveNodeFromArrays(VoxelTreeElement* node) {

    if (!_initialized) {
        return 0;
    }

    if (_usePrimitiveRenderer) {
        if (node->isKnownBufferIndex()) {
            int primitiveIndex = node->getBufferIndex();
            _renderer->remove(primitiveIndex);
            node->setBufferIndex(GLBUFFER_INDEX_UNKNOWN);
            return 1;
        }
    } else {
        // if the node is not in the VBOs then we have nothing to do!
        if (node->isKnownBufferIndex()) {
            // If this node has not yet been written to the array, then add it to the end of the array.
            glBufferIndex nodeIndex = node->getBufferIndex();
            node->setBufferIndex(GLBUFFER_INDEX_UNKNOWN);
            freeBufferIndex(nodeIndex); // NOTE: This will make the node invisible!
            return 1; // updated!
        }
    }
    return 0; // not-updated
}

int VoxelSystem::updateNodeInArrays(VoxelTreeElement* node, bool reuseIndex, bool forceDraw) {
    // If we've run out of room, then just bail...
    if (_voxelsInWriteArrays >= _maxVoxels && (_freeIndexes.size() == 0)) {
        // We need to think about what else we can do in this case. This basically means that all of our available
        // VBO slots are used up, but we're trying to render more voxels. At this point, if this happens we'll just
        // not render these Voxels. We need to think about ways to keep the entire scene intact but maybe lower quality
        // possibly shifting down to lower LOD or something. This debug message is to help identify, if/when/how this
        // state actually occurs.
        if (Application::getInstance()->getLogger()->extraDebugging()) {
            qDebug("OH NO! updateNodeInArrays() BAILING (_voxelsInWriteArrays >= _maxVoxels)");
        }
        return 0;
    }

    if (!_initialized) {
        return 0;
    }

    // If we've changed any attributes (our renderness, our color, etc), or we've been told to force a redraw
    // then update the Arrays...
    if (forceDraw || node->isDirty()) {
        // If we're should render, use our legit location and scale,
        if (node->getShouldRender()) {
            glm::vec3 startVertex = node->getCorner();
            float voxelScale = node->getScale();
            nodeColor const & color = node->getColor();

            if (_usePrimitiveRenderer) {
                if (node->isKnownBufferIndex()) {
                    int primitiveIndex = node->getBufferIndex();
                    _renderer->remove(primitiveIndex);
                    node->setBufferIndex(GLBUFFER_INDEX_UNKNOWN);
                } else {
                    node->setVoxelSystem(this);
                }
                unsigned char occlusions;
                if (_showCulledSharedFaces) {
                    occlusions = ~node->getInteriorOcclusions();
                } else {
                    occlusions = node->getInteriorOcclusions();
                }
                if (occlusions != OctreeElement::HalfSpace::All) {
                    Cube* cube = new Cube(
                        startVertex.x, startVertex.y, startVertex.z, voxelScale, 
                        color[RED_INDEX], color[GREEN_INDEX], color[BLUE_INDEX],
                        occlusions);
                    if (cube) {
                        int primitiveIndex = _renderer->add(cube);
                        node->setBufferIndex(primitiveIndex);
                    }
                }
            } else {
                glBufferIndex nodeIndex = GLBUFFER_INDEX_UNKNOWN;
                if (reuseIndex && node->isKnownBufferIndex()) {
                    nodeIndex = node->getBufferIndex();
                } else {
                    nodeIndex = getNextBufferIndex();
                    node->setBufferIndex(nodeIndex);
                    node->setVoxelSystem(this);
                }
                
                // populate the array with points for the 8 vertices and RGB color for each added vertex
                updateArraysDetails(nodeIndex, startVertex, voxelScale, node->getColor());
            }
            return 1; // updated!
        } else {
            // If we shouldn't render, and we're in reuseIndex mode, then free our index, this only operates
            // on nodes with known index values, so it's safe to call for any node.
            if (reuseIndex) {
                return forceRemoveNodeFromArrays(node);
            }
        }
    }
    return 0; // not-updated
}

void VoxelSystem::updateArraysDetails(glBufferIndex nodeIndex, const glm::vec3& startVertex,
                                     float voxelScale, const nodeColor& color) {
    
    if (_initialized && nodeIndex <= _maxVoxels) {
        _writeVoxelDirtyArray[nodeIndex] = true;
        
        if (_useVoxelShader) {
            // write in position, scale, and color for the voxel
            if (_writeVoxelShaderData) {
                VoxelShaderVBOData* writeVerticesAt = &_writeVoxelShaderData[nodeIndex];
                writeVerticesAt->x = startVertex.x * TREE_SCALE;
                writeVerticesAt->y = startVertex.y * TREE_SCALE;
                writeVerticesAt->z = startVertex.z * TREE_SCALE;
                writeVerticesAt->s = voxelScale * TREE_SCALE;
                writeVerticesAt->r = color[RED_INDEX];
                writeVerticesAt->g = color[GREEN_INDEX];
                writeVerticesAt->b = color[BLUE_INDEX];
            }
            
        } else {
            if (_writeVerticesArray && _writeColorsArray) {
                int vertexPointsPerVoxel = GLOBAL_NORMALS_VERTEX_POINTS_PER_VOXEL;
                for (int j = 0; j < vertexPointsPerVoxel; j++ ) {
                    GLfloat* writeVerticesAt = _writeVerticesArray + (nodeIndex * vertexPointsPerVoxel);
                    GLubyte* writeColorsAt   = _writeColorsArray   + (nodeIndex * vertexPointsPerVoxel);
                    *(writeVerticesAt+j) = startVertex[j % 3] + (identityVerticesGlobalNormals[j] * voxelScale);
                    *(writeColorsAt  +j) = color[j % 3];
                }
            }
        }
    
    }
}

glm::vec3 VoxelSystem::computeVoxelVertex(const glm::vec3& startVertex, float voxelScale, int index) const {
    const float* identityVertex = identityVertices + index * 3;
    return startVertex + glm::vec3(identityVertex[0], identityVertex[1], identityVertex[2]) * voxelScale;
}

ProgramObject VoxelSystem::_perlinModulateProgram;
ProgramObject VoxelSystem::_shadowMapProgram;
ProgramObject VoxelSystem::_cascadedShadowMapProgram;
int VoxelSystem::_shadowDistancesLocation;

void VoxelSystem::init() {
    if (_initialized) {
        qDebug("[ERROR] VoxelSystem is already initialized.");
        return;
    }

    _callsToTreesToArrays = 0;
    _setupNewVoxelsForDrawingLastFinished = 0;
    _setupNewVoxelsForDrawingLastElapsed = 0;
    _lastViewCullingElapsed = _lastViewCulling = _lastAudit = _lastViewIsChanging = 0;
    _hasRecentlyChanged = false;

    _voxelsDirty = false;
    _voxelsInWriteArrays = 0;
    _voxelsInReadArrays = 0;

    // VBO for the verticesArray
    _initialMemoryUsageGPU = getFreeMemoryGPU();
    initVoxelMemory();

    // our own _removedVoxels doesn't need to be notified of voxel deletes
    VoxelTreeElement::removeDeleteHook(&_removedVoxels);

}

void VoxelSystem::changeTree(VoxelTree* newTree) {
    _tree = newTree;

    _tree->setDirtyBit();
    _tree->getRoot()->setVoxelSystem(this);

    setupNewVoxelsForDrawing();
}

void VoxelSystem::updateFullVBOs() {
    bool outputWarning = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(outputWarning, "updateFullVBOs()");

    {
        static char buffer[128] = { 0 };
        if (outputWarning) {
            sprintf(buffer, "updateFullVBOs() : updateVBOSegment(0, _voxelsInReadArrays=%lu);", _voxelsInReadArrays);
        };

        PerformanceWarning warn(outputWarning,buffer);
        updateVBOSegment(0, _voxelsInReadArrays);
    }

    {
        PerformanceWarning warn(outputWarning,"updateFullVBOs() : memset(_readVoxelDirtyArray...)");
        // consider the _readVoxelDirtyArray[] clean!
        memset(_readVoxelDirtyArray, false, _voxelsInReadArrays * sizeof(bool));
    }
}

void VoxelSystem::updatePartialVBOs() {
    glBufferIndex segmentStart = 0;
    bool inSegment = false;
    for (glBufferIndex i = 0; i < _voxelsInReadArrays; i++) {
        bool thisVoxelDirty = _readVoxelDirtyArray[i];
        if (!inSegment) {
            if (thisVoxelDirty) {
                segmentStart = i;
                inSegment = true;
                _readVoxelDirtyArray[i] = false; // consider us clean!
            }
        } else {
            if (!thisVoxelDirty) {
                // If we got here because because this voxel is NOT dirty, so the last dirty voxel was the one before
                // this one and so that's where the "segment" ends
                updateVBOSegment(segmentStart, i - 1);
                inSegment = false;
            }
            _readVoxelDirtyArray[i] = false; // consider us clean!
        }
    }

    // if we got to the end of the array, and we're in an active dirty segment...
    if (inSegment) {
        updateVBOSegment(segmentStart, _voxelsInReadArrays - 1);
        inSegment = false;
    }
}

void VoxelSystem::updateVBOs() {
    static char buffer[40] = { 0 };
    if (Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings)) {
        sprintf(buffer, "updateVBOs() _readRenderFullVBO=%s", debug::valueOf(_readRenderFullVBO));
    };
    // would like to include _callsToTreesToArrays
    PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings), buffer);
    if (! _usePrimitiveRenderer) {
        if (_voxelsDirty) {
    
            // attempt to lock the read arrays, to for copying from them to the actual GPU VBOs.
            // if we fail to get the lock, that's ok, our VBOs will update on the next frame...
            const int WAIT_FOR_LOCK_IN_MS = 5;
            if (_readArraysLock.tryLockForRead(WAIT_FOR_LOCK_IN_MS)) {
                if (_readRenderFullVBO) {
                    updateFullVBOs();
                } else {
                    updatePartialVBOs();
                }
                _voxelsDirty = false;
                _readRenderFullVBO = false;
                _readArraysLock.unlock();
            } else {
                qDebug() << "updateVBOs().... couldn't get _readArraysLock.tryLockForRead()";
            }
        }
    }
    _callsToTreesToArrays = 0; // clear it
}

// this should only be called on the main application thread during render
void VoxelSystem::updateVBOSegment(glBufferIndex segmentStart, glBufferIndex segmentEnd) {
    bool showWarning = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showWarning, "updateVBOSegment()");

    if (_useVoxelShader) {
        int segmentLength = (segmentEnd - segmentStart) + 1;
        
#if 0
        GLintptr segmentStartAt = segmentStart * sizeof(VoxelShaderVBOData);
        GLsizeiptr segmentSizeBytes = segmentLength * sizeof(VoxelShaderVBOData);

        void* readVerticesFrom = &_readVoxelShaderData[segmentStart];
        glBindBuffer(GL_ARRAY_BUFFER, _vboVoxelsID);
        glBufferSubData(GL_ARRAY_BUFFER, segmentStartAt, segmentSizeBytes, readVerticesFrom);
#endif // #if 0
        
        GLintptr segmentStartAt = segmentStart * sizeof(VoxelInstanceShaderVBOData);
        GLsizeiptr segmentSizeBytes = segmentLength * sizeof(VoxelInstanceShaderVBOData);
        
        void* readVerticesFrom = &_outputInstanceVoxelData[segmentStart];
        glBindBuffer(GL_ARRAY_BUFFER, _voxelInfoID);
        glBufferSubData(GL_ARRAY_BUFFER, segmentStartAt, segmentSizeBytes, readVerticesFrom);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        
    } else {
        int vertexPointsPerVoxel = GLOBAL_NORMALS_VERTEX_POINTS_PER_VOXEL;
        int segmentLength = (segmentEnd - segmentStart) + 1;
        GLintptr   segmentStartAt   = segmentStart * vertexPointsPerVoxel * sizeof(GLfloat);
        GLsizeiptr segmentSizeBytes = segmentLength * vertexPointsPerVoxel * sizeof(GLfloat);
        GLfloat* readVerticesFrom   = _readVerticesArray + (segmentStart * vertexPointsPerVoxel);

        {
            PerformanceWarning warn(showWarning, "updateVBOSegment() : glBindBuffer(GL_ARRAY_BUFFER, _vboVerticesID);");
            glBindBuffer(GL_ARRAY_BUFFER, _vboVerticesID);
        }

        {
            PerformanceWarning warn(showWarning, "updateVBOSegment() : glBufferSubData() _vboVerticesID);");
            glBufferSubData(GL_ARRAY_BUFFER, segmentStartAt, segmentSizeBytes, readVerticesFrom);
        }

        segmentStartAt          = segmentStart * vertexPointsPerVoxel * sizeof(GLubyte);
        segmentSizeBytes        = segmentLength * vertexPointsPerVoxel * sizeof(GLubyte);
        GLubyte* readColorsFrom = _readColorsArray   + (segmentStart * vertexPointsPerVoxel);

        {
            PerformanceWarning warn(showWarning, "updateVBOSegment() : glBindBuffer(GL_ARRAY_BUFFER, _vboColorsID);");
            glBindBuffer(GL_ARRAY_BUFFER, _vboColorsID);
        }

        {
            PerformanceWarning warn(showWarning, "updateVBOSegment() : glBufferSubData() _vboColorsID);");
            glBufferSubData(GL_ARRAY_BUFFER, segmentStartAt, segmentSizeBytes, readColorsFrom);
        }
    }
}

void VoxelSystem::render() {
    bool texture = Menu::getInstance()->isOptionChecked(MenuOption::VoxelTextures);
    bool showWarnings = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showWarnings, "render()");

    // If we got here and we're not initialized then bail!
    if (!_initialized) {
        return;
    }

    updateVBOs();

    // if not don't... then do...
    if (_useVoxelShader) {
        PerformanceWarning warn(showWarnings,"render().. _useVoxelShader openGL..");
        
        if (_voxelModelID) {
            _voxelInstanceProgram.bind();
            
            size_t strideSize = sizeof(GLfloat) * 4;
            
            // voxel info
            glBindBuffer(GL_ARRAY_BUFFER, _voxelInfoID);
            glEnableClientState(GL_VERTEX_ARRAY);

            // translation
            glVertexAttribPointer(_translationShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, sizeof(VoxelInstanceShaderVBOData), 0);
            glEnableVertexAttribArray(_translationShaderAttributeLocation);
            glVertexAttribDivisorARB(_translationShaderAttributeLocation, 1);
            
            // scale
            glVertexAttribPointer(_scaleShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, sizeof(VoxelInstanceShaderVBOData), (void *)strideSize);
            glEnableVertexAttribArray(_scaleShaderAttributeLocation);
            glVertexAttribDivisorARB(_scaleShaderAttributeLocation, 1);
            
            // color
            glVertexAttribPointer(_colorShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, sizeof(VoxelInstanceShaderVBOData), (void *)(strideSize * 2));
            glEnableVertexAttribArray(_colorShaderAttributeLocation);
            glVertexAttribDivisorARB(_colorShaderAttributeLocation, 1);

            // voxel model
            glBindBuffer(GL_ARRAY_BUFFER, _voxelModelID);
            glEnableClientState(GL_VERTEX_ARRAY);
            
            strideSize = sizeof(float) * 4 * 2 + sizeof(float) * 2;
            
            // position
            glVertexAttribPointer(_positionShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, strideSize, 0);
            glEnableVertexAttribArray(_positionShaderAttributeLocation);
            
            // normal
            glVertexAttribPointer(_normalShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, strideSize, (void *)(sizeof(float) * 4));
            glEnableVertexAttribArray(_normalShaderAttributeLocation);
            
            // uv
            glVertexAttribPointer(_uvShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, strideSize, (void *)(sizeof(float) * 4 * 2));
            glEnableVertexAttribArray(_uvShaderAttributeLocation);
            
//GLfloat modelViewMatrixFloatVal[16];
//glGetFloatv(GL_MODELVIEW_MATRIX, modelViewMatrixFloatVal);
//
//GLfloat projectionMatrixFloatVal[16];
//glGetFloatv(GL_PROJECTION_MATRIX, projectionMatrixFloatVal);
//
//glm::mat4 modelViewMatrix = glm::make_mat4(modelViewMatrixFloatVal);
//glm::mat4 projectionMatrix = glm::make_mat4(projectionMatrixFloatVal);
//
//glm::vec4 test( 0.0f, 0.0f, 0.0, 1.0);
//glm::vec4 testResult = modelViewMatrix * test;
//glm::vec4 projected = projectionMatrix * testResult;
//
//projected.x /= projected.w;
//projected.y /= projected.w;
//projected.z /= projected.w;
            
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _voxelModelIndicesID);
        
            glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0, _voxelsInReadArrays);
            
            glDisableClientState(GL_VERTEX_ARRAY);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            
            glVertexAttribDivisorARB(_translationShaderAttributeLocation, 0);
            glVertexAttribDivisorARB(_scaleShaderAttributeLocation, 0);
            glVertexAttribDivisorARB(_colorShaderAttributeLocation, 0);
            
            glDisableVertexAttribArray(_translationShaderAttributeLocation);
            glDisableVertexAttribArray(_scaleShaderAttributeLocation);
            glDisableVertexAttribArray(_colorShaderAttributeLocation);
            
            glDisableVertexAttribArray(_positionShaderAttributeLocation);
            glDisableVertexAttribArray(_normalShaderAttributeLocation);
            glDisableVertexAttribArray(_uvShaderAttributeLocation);
            
            _voxelInstanceProgram.release();
        }
        
#if 0
        //Define this somewhere in your header file
        #define BUFFER_OFFSET(i) ((void*)(i))

        glBindBuffer(GL_ARRAY_BUFFER, _vboVoxelsID);
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(VoxelShaderVBOData), BUFFER_OFFSET(0));   //The starting point of the VBO, for the vertices

        int attributeLocation;

        if (!_voxelsAsPoints) {
            Application::getInstance()->getVoxelShader().begin();
            attributeLocation = Application::getInstance()->getVoxelShader().attributeLocation("voxelSizeIn");
            glEnableVertexAttribArray(attributeLocation);
            glVertexAttribPointer(attributeLocation, 1, GL_FLOAT, false, sizeof(VoxelShaderVBOData), BUFFER_OFFSET(3*sizeof(float)));
        } else {
            glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

            glm::vec2 viewDimensions = Application::getInstance()->getViewportDimensions();
            float viewportWidth = viewDimensions.x;
            float viewportHeight = viewDimensions.y;
            glm::vec3 cameraPosition = Application::getInstance()->getViewFrustum()->getPosition();
            PointShader& pointShader = Application::getInstance()->getPointShader();

            pointShader.begin();

            pointShader.setUniformValue(pointShader.uniformLocation("viewportWidth"), viewportWidth);
            pointShader.setUniformValue(pointShader.uniformLocation("viewportHeight"), viewportHeight);
            pointShader.setUniformValue(pointShader.uniformLocation("cameraPosition"), cameraPosition);

            attributeLocation = pointShader.attributeLocation("voxelSizeIn");
            glEnableVertexAttribArray(attributeLocation);
            glVertexAttribPointer(attributeLocation, 1, GL_FLOAT, false, sizeof(VoxelShaderVBOData), BUFFER_OFFSET(3*sizeof(float)));
        }


        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(3, GL_UNSIGNED_BYTE, sizeof(VoxelShaderVBOData), BUFFER_OFFSET(4*sizeof(float)));//The starting point of colors

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _vboVoxelsIndicesID);

        glDrawElements(GL_POINTS, _voxelsInReadArrays, GL_UNSIGNED_INT, BUFFER_OFFSET(0)); //The starting point of the IBO

        // deactivate vertex and color arrays after drawing
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);

        // bind with 0 to switch back to normal operation
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        if (!_voxelsAsPoints) {
            Application::getInstance()->getVoxelShader().end();
            glDisableVertexAttribArray(attributeLocation);
        } else {
            Application::getInstance()->getPointShader().end();
            glDisableVertexAttribArray(attributeLocation);
            glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
        }
#endif // #if 0
        
    } else 
    if (!_usePrimitiveRenderer) {
        if (_drawHaze) {
            glEnable(GL_FOG);
        }

        PerformanceWarning warn(showWarnings, "render().. TRIANGLES...");

        {
            PerformanceWarning warn(showWarnings,"render().. setup before glDrawRangeElementsEXT()...");

            // tell OpenGL where to find vertex and color information
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);

            glBindBuffer(GL_ARRAY_BUFFER, _vboVerticesID);
            glVertexPointer(3, GL_FLOAT, 0, 0);

            glBindBuffer(GL_ARRAY_BUFFER, _vboColorsID);
            glColorPointer(3, GL_UNSIGNED_BYTE, 0, 0);

            applyScaleAndBindProgram(texture);

            // for performance, enable backface culling
            glEnable(GL_CULL_FACE);
        }

        // draw voxels in 6 passes

        {
            PerformanceWarning warn(showWarnings, "render().. glDrawRangeElementsEXT()...");

            glNormal3f(0,1.0f,0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _vboIndicesTop);
            glDrawRangeElementsEXT(GL_TRIANGLES, 0, GLOBAL_NORMALS_VERTICES_PER_VOXEL * _voxelsInReadArrays - 1,
                INDICES_PER_FACE * _voxelsInReadArrays, GL_UNSIGNED_INT, 0);

            glNormal3f(0,-1.0f,0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _vboIndicesBottom);
            glDrawRangeElementsEXT(GL_TRIANGLES, 0, GLOBAL_NORMALS_VERTICES_PER_VOXEL * _voxelsInReadArrays - 1,
                INDICES_PER_FACE * _voxelsInReadArrays, GL_UNSIGNED_INT, 0);

            glNormal3f(-1.0f,0,0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _vboIndicesLeft);
            glDrawRangeElementsEXT(GL_TRIANGLES, 0, GLOBAL_NORMALS_VERTICES_PER_VOXEL * _voxelsInReadArrays - 1,
                INDICES_PER_FACE * _voxelsInReadArrays, GL_UNSIGNED_INT, 0);

            glNormal3f(1.0f,0,0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _vboIndicesRight);
            glDrawRangeElementsEXT(GL_TRIANGLES, 0, GLOBAL_NORMALS_VERTICES_PER_VOXEL * _voxelsInReadArrays - 1,
                INDICES_PER_FACE * _voxelsInReadArrays, GL_UNSIGNED_INT, 0);

            glNormal3f(0,0,-1.0f);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _vboIndicesFront);
            glDrawRangeElementsEXT(GL_TRIANGLES, 0, GLOBAL_NORMALS_VERTICES_PER_VOXEL * _voxelsInReadArrays - 1,
                INDICES_PER_FACE * _voxelsInReadArrays, GL_UNSIGNED_INT, 0);

            glNormal3f(0,0,1.0f);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _vboIndicesBack);
            glDrawRangeElementsEXT(GL_TRIANGLES, 0, GLOBAL_NORMALS_VERTICES_PER_VOXEL * _voxelsInReadArrays - 1,
                INDICES_PER_FACE * _voxelsInReadArrays, GL_UNSIGNED_INT, 0);
        }
        {
            PerformanceWarning warn(showWarnings, "render().. cleanup after glDrawRangeElementsEXT()...");

            glDisable(GL_CULL_FACE);

            removeScaleAndReleaseProgram(texture);

            // deactivate vertex and color arrays after drawing
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);

            // bind with 0 to switch back to normal operation
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
        
        if (_drawHaze) {
            glDisable(GL_FOG);
        }
    }
    else {
        applyScaleAndBindProgram(texture);
        _renderer->render();
        removeScaleAndReleaseProgram(texture);

    }
}

void VoxelSystem::applyScaleAndBindProgram(bool texture) {

    if (Menu::getInstance()->getShadowsEnabled()) {
        if (Menu::getInstance()->isOptionChecked(MenuOption::CascadedShadows)) {
            _cascadedShadowMapProgram.bind();
            _cascadedShadowMapProgram.setUniform(_shadowDistancesLocation, Application::getInstance()->getShadowDistances());
        } else {
            _shadowMapProgram.bind();
        }
        glBindTexture(GL_TEXTURE_2D, Application::getInstance()->getTextureCache()->getShadowDepthTextureID());

    } else if (texture) {
        bindPerlinModulateProgram();
        glBindTexture(GL_TEXTURE_2D, Application::getInstance()->getTextureCache()->getPermutationNormalTextureID());
    }

    glPushMatrix();
    glScalef(_treeScale, _treeScale, _treeScale);
}

void VoxelSystem::removeScaleAndReleaseProgram(bool texture) {
    // scale back down to 1 so heads aren't massive
    glPopMatrix();

    if (Menu::getInstance()->getShadowsEnabled()) {
        if (Menu::getInstance()->isOptionChecked(MenuOption::CascadedShadows)) {
            _cascadedShadowMapProgram.release();
        } else {
            _shadowMapProgram.release();
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        
    } else if (texture) {
        _perlinModulateProgram.release();
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

int VoxelSystem::_nodeCount = 0;

void VoxelSystem::killLocalVoxels() {
    PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings), 
                            "VoxelSystem::killLocalVoxels()");
    _tree->lockForWrite();
    VoxelSystem* voxelSystem = _tree->getRoot()->getVoxelSystem();
    _tree->eraseAllOctreeElements();
    _tree->getRoot()->setVoxelSystem(voxelSystem);
    _tree->unlock();
    clearFreeBufferIndexes();
    if (_usePrimitiveRenderer) {
        if (_renderer) {
            _renderer->release();
        }
        clearAllNodesBufferIndex();
    }
    _voxelsInReadArrays = 0; // do we need to do this?
    setupNewVoxelsForDrawing();
}

// only called on main thread
bool VoxelSystem::clearAllNodesBufferIndexOperation(OctreeElement* element, void* extraData) {
    _nodeCount++;
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;
    voxel->setBufferIndex(GLBUFFER_INDEX_UNKNOWN);
    return true;
}

// only called on main thread, and also always followed by a call to cleanupVoxelMemory()
// you shouldn't be calling this on any other thread or without also cleaning up voxel memory
void VoxelSystem::clearAllNodesBufferIndex() {
    PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings), 
                            "VoxelSystem::clearAllNodesBufferIndex()");
    _nodeCount = 0;
    _tree->lockForRead(); // we won't change the tree so it's ok to treat this as a read
    _tree->recurseTreeWithOperation(clearAllNodesBufferIndexOperation);
    clearFreeBufferIndexes(); // this should be called too
    _tree->unlock();
    if (Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings)) {
        qDebug("clearing buffer index of %d nodes", _nodeCount);
    }
}

bool VoxelSystem::inspectForInteriorOcclusionsOperation(OctreeElement* element, void* extraData) {
    _nodeCount++;
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;

    // Nothing to do at the leaf level
    if (voxel->isLeaf()) {
        return false;
    }

    // Bit mask of occluded shared faces indexed by child
    unsigned char occludedSharedFace[NUMBER_OF_CHILDREN] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    // Traverse all pair combinations of children
    for (int i = NUMBER_OF_CHILDREN; --i >= 0; ) {

        VoxelTreeElement* childA = voxel->getChildAtIndex(i);
        if (childA) {

            // Get the child A's occluding faces, for a leaf that will be
            // all six voxel faces, and for a non leaf, that will be
            // all faces which are completely covered by four child octants.
            unsigned char exteriorOcclusionsA = childA->getExteriorOcclusions();

            for (int j = i; --j >= 0; ) {

                VoxelTreeElement* childB = voxel->getChildAtIndex(j);
                if (childB) {

                    // Get child B's occluding faces
                    unsigned char exteriorOcclusionsB = childB->getExteriorOcclusions();

                    // Determine the shared halfspace partition between siblings A and B,
                    // i.e., near/far, left/right, or top/bottom
                    unsigned char partitionA = _sOctantIndexToSharedBitMask[i][j] &
                                                exteriorOcclusionsA;
                    unsigned char partitionB = _sOctantIndexToSharedBitMask[i][j] &
                                                exteriorOcclusionsB;

                    // Determine which face of each sibling is occluded.
  
                    // The _sOctantIndexToBitMask is a partition occupancy mask. For
                    // example, if the near-left-top (NLT) and near-left-bottom (NLB) child voxels
                    // exist, the shared partition is top-bottom (TB), and thus the occluded
                    // shared face of the NLT voxel is its bottom face.
                    occludedSharedFace[i] |= (partitionB & _sOctantIndexToBitMask[i]);
                    occludedSharedFace[j] |= (partitionA & _sOctantIndexToBitMask[j]);
                }
            }
            // Exchange bit pairs, left to right, vice versa, etc.
            occludedSharedFace[i] = _sSwizzledOcclusionBits[occludedSharedFace[i]];
            // Combine this voxel's interior excluded shared face only to those children which are coincident
            // with the excluded face.
            occludedSharedFace[i] |= (voxel->getInteriorOcclusions() & _sOctantIndexToBitMask[i]);

            // Inform the child
            childA->setInteriorOcclusions(occludedSharedFace[i]);
            if (occludedSharedFace[i] != OctreeElement::HalfSpace::None) {
                //const glm::vec3& v = voxel->getCorner();
                //float s = voxel->getScale();

                //qDebug("Child %d of voxel at %f %f %f size: %f has %02x occlusions", i, v.x, v.y, v.z, s, occludedSharedFace[i]);
            }
        }
    }
    return true;
}

bool VoxelSystem::inspectForExteriorOcclusionsOperation(OctreeElement* element, void* extraData) {
    _nodeCount++;
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;

    // Nothing to do at the leaf level
    if (voxel->isLeaf()) {
        // By definition the the exterior faces of a leaf voxel are
        // always occluders.
        voxel->setExteriorOcclusions(OctreeElement::HalfSpace::All);
        // And the sibling occluders
        voxel->setInteriorOcclusions(OctreeElement::HalfSpace::None);
        return false;
    } else {
        voxel->setExteriorOcclusions(OctreeElement::HalfSpace::None);
        voxel->setInteriorOcclusions(OctreeElement::HalfSpace::None);
    }

    // Count of exterior occluding faces of this voxel element indexed 
    // by half space partition
    unsigned int exteriorOcclusionsCt[6]   = { 0, 0, 0, 0, 0, 0 };

    // Traverse all children
    for (int i = NUMBER_OF_CHILDREN; --i >= 0; ) {

        VoxelTreeElement* child = voxel->getChildAtIndex(i);
        if (child) {

               // Get the child's occluding faces, for a leaf, that will be
                // all six voxel faces, and for a non leaf, that will be
                // all faces which are completely covered by four child octants.
                unsigned char exteriorOcclusionsOfChild = child->getExteriorOcclusions();
                exteriorOcclusionsOfChild &= _sOctantIndexToBitMask[i];

                for (int j = 6; --j >= 0; ) {

                    // Determine if the halfspace partition indexed by 1 << j is
                    // present in the exterior occlusions of the child.
                    unsigned char partition = exteriorOcclusionsOfChild & (1 << j);

                    if (partition) {
                        exteriorOcclusionsCt[j]++;
                    }
                }
        }
    }
    {
        // Derive the exterior occlusions of the voxel elements from the exclusions
        // of its children
        unsigned char exteriorOcclusions = OctreeElement::HalfSpace::None;
        for (int i = 6; --i >= 0; ) {
            if (exteriorOcclusionsCt[i] == _sNumOctantsPerHemiVoxel) {

                // Exactly four octants qualify for full exterior occlusion
                exteriorOcclusions |= (1 << i);
            }
        }

        // Inform the voxel element
        voxel->setExteriorOcclusions(exteriorOcclusions);

        if (exteriorOcclusions == OctreeElement::HalfSpace::All) {
            //const glm::vec3& v = voxel->getCorner();
            //float s = voxel->getScale();

            //qDebug("Completely occupied voxel at %f %f %f size: %f", v.x, v.y, v.z, s);

            // All of the exterior faces of this voxel element are
            // occluders, which means that this element is completely
            // occupied. Hence, the subtree from this node could be
            // pruned and replaced by a leaf voxel, if the visible 
            // properties of the children are the same

        } else if (exteriorOcclusions != OctreeElement::HalfSpace::None) {
            //const glm::vec3& v = voxel->getCorner();
            //float s = voxel->getScale();

            //qDebug("Partially occupied voxel at %f %f %f size: %f with %02x", v.x, v.y, v.z, s, exteriorOcclusions);
        }
    }
    return true;
}

void VoxelSystem::inspectForOcclusions() {

    if (_inOcclusions) {
        return;
    }
    _inOcclusions = true;
    _nodeCount = 0;

    bool showDebugDetails = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showDebugDetails, "inspectForOcclusions()");

    _tree->lockForRead();
    _tree->recurseTreeWithPostOperation(inspectForExteriorOcclusionsOperation);
    _nodeCount = 0;
    _tree->recurseTreeWithOperation(inspectForInteriorOcclusionsOperation);
    _tree->unlock();

    if (showDebugDetails) {
        qDebug("inspecting all occlusions of %d nodes", _nodeCount);
    }
    _inOcclusions = false;
}

bool VoxelSystem::forceRedrawEntireTreeOperation(OctreeElement* element, void* extraData) {
    _nodeCount++;
    element->setDirtyBit();
    return true;
}

void VoxelSystem::forceRedrawEntireTree() {
    _nodeCount = 0;
    _tree->recurseTreeWithOperation(forceRedrawEntireTreeOperation);
    qDebug("forcing redraw of %d nodes", _nodeCount);
    _tree->setDirtyBit();
    setupNewVoxelsForDrawing();
}

bool VoxelSystem::isViewChanging() {
    bool result = false; // assume the best

    // If our viewFrustum has changed since our _lastKnownViewFrustum
    if (!_lastKnownViewFrustum.isVerySimilar(_viewFrustum)) {
        result = true;
        _lastKnownViewFrustum = *_viewFrustum; // save last known
    }
    return result;
}

bool VoxelSystem::hasViewChanged() {
    bool result = false; // assume the best

    // If we're still changing, report no change yet.
    if (isViewChanging()) {
        return false;
    }

    // If our viewFrustum has changed since our _lastKnownViewFrustum
    if (!_lastStableViewFrustum.isVerySimilar(_viewFrustum)) {
        result = true;
        _lastStableViewFrustum = *_viewFrustum; // save last stable
    }
    return result;
}

// combines the removeOutOfView args into a single class
class hideOutOfViewArgs {
public:
    VoxelSystem* thisVoxelSystem;
    VoxelTree* tree;
    ViewFrustum thisViewFrustum;
    ViewFrustum lastViewFrustum;
    bool culledOnce;
    bool wantDeltaFrustums;
    unsigned long nodesScanned;
    unsigned long nodesRemoved;
    unsigned long nodesInside;
    unsigned long nodesIntersect;
    unsigned long nodesOutside;
    unsigned long nodesInsideInside;
    unsigned long nodesIntersectInside;
    unsigned long nodesOutsideInside;
    unsigned long nodesInsideOutside;
    unsigned long nodesOutsideOutside;
    unsigned long nodesShown;

    hideOutOfViewArgs(VoxelSystem* voxelSystem, VoxelTree* tree,
                        bool culledOnce, bool widenViewFrustum, bool wantDeltaFrustums) :
        thisVoxelSystem(voxelSystem),
        tree(tree),
        thisViewFrustum(*voxelSystem->getViewFrustum()),
        lastViewFrustum(*voxelSystem->getLastCulledViewFrustum()),
        culledOnce(culledOnce),
        wantDeltaFrustums(wantDeltaFrustums),
        nodesScanned(0),
        nodesRemoved(0),
        nodesInside(0),
        nodesIntersect(0),
        nodesOutside(0),
        nodesInsideInside(0),
        nodesIntersectInside(0),
        nodesOutsideInside(0),
        nodesInsideOutside(0),
        nodesOutsideOutside(0),
        nodesShown(0)
    {
        // Widen the FOV for trimming
        if (widenViewFrustum) {
            float originalFOV = thisViewFrustum.getFieldOfView();
            float wideFOV = originalFOV + VIEW_FRUSTUM_FOV_OVERSEND;
            thisViewFrustum.setFieldOfView(wideFOV);
            thisViewFrustum.calculate();
        }
    }
};

void VoxelSystem::hideOutOfView(bool forceFullFrustum) {

    // don't re-enter...
    if (_inhideOutOfView) {
        return;
    }

    _inhideOutOfView = true;

    bool showDebugDetails = Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings);
    PerformanceWarning warn(showDebugDetails, "hideOutOfView()");
    bool widenFrustum = true;


    // When using "delta" view frustums and only hide/show items that are in the difference
    // between the two view frustums. There are some potential problems with this mode.
    //
    // 1) This work well for rotating, but what about moving forward?
    //    In the move forward case, you'll get new voxel details, but those
    //    new voxels will be in the last view.
    //
    // 2) Also, voxels will arrive from the network that are OUTSIDE of the view
    //    frustum... these won't get hidden... and so we can't assume they are correctly
    //    hidden...
    //
    // Both these problems are solved by intermittently calling this with forceFullFrustum set
    // to true. This will essentially clean up the improperly hidden or shown voxels.
    //
    bool wantDeltaFrustums = !forceFullFrustum;
    hideOutOfViewArgs args(this, this->_tree, _culledOnce, widenFrustum, wantDeltaFrustums);

    const bool wantViewFrustumDebugging = false; // change to true for additional debugging
    if (wantViewFrustumDebugging) {
        args.thisViewFrustum.printDebugDetails();
        if (_culledOnce) {
            args.lastViewFrustum.printDebugDetails();
        }
    }

    if (!forceFullFrustum && _culledOnce && args.lastViewFrustum.isVerySimilar(args.thisViewFrustum)) {
        _inhideOutOfView = false;
        return;
    }

    {
        PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings), 
                            "VoxelSystem::... recurseTreeWithOperation(hideOutOfViewOperation)");
        _tree->lockForRead();
        _tree->recurseTreeWithOperation(hideOutOfViewOperation,(void*)&args);
        _tree->unlock();
    }
    _lastCulledViewFrustum = args.thisViewFrustum; // save last stable
    _culledOnce = true;

    if (args.nodesRemoved) {
        _tree->setDirtyBit();
        setupNewVoxelsForDrawingSingleNode(DONT_BAIL_EARLY);
    }

    bool extraDebugDetails = false; // Application::getInstance()->getLogger()->extraDebugging();
    if (extraDebugDetails) {
        qDebug("hideOutOfView() scanned=%ld removed=%ld show=%ld inside=%ld intersect=%ld outside=%ld",
                args.nodesScanned, args.nodesRemoved, args.nodesShown, args.nodesInside,
                args.nodesIntersect, args.nodesOutside
            );
        qDebug("inside/inside=%ld intersect/inside=%ld outside/outside=%ld",
                args.nodesInsideInside, args.nodesIntersectInside, args.nodesOutsideOutside
            );

        qDebug() << "args.thisViewFrustum....";
        args.thisViewFrustum.printDebugDetails();
    }
    _inhideOutOfView = false;
}

bool VoxelSystem::hideAllSubTreeOperation(OctreeElement* element, void* extraData) {
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;
    hideOutOfViewArgs* args = (hideOutOfViewArgs*)extraData;
    
    // If we've culled at least once, then we will use the status of this voxel in the last culled frustum to determine
    // how to proceed. If we've never culled, then we just consider all these voxels to be UNKNOWN so that we will not
    // consider that case.
    ViewFrustum::location inLastCulledFrustum;

    if (args->culledOnce && args->wantDeltaFrustums) {
        inLastCulledFrustum = voxel->inFrustum(args->lastViewFrustum);

        // if this node is fully OUTSIDE our last culled view frustum, then we don't need to recurse further
        if (inLastCulledFrustum == ViewFrustum::OUTSIDE) {
            args->nodesOutsideOutside++;
            return false;
        }
    }

    args->nodesOutside++;
    if (voxel->isKnownBufferIndex()) {
        args->nodesRemoved++;
        VoxelSystem* thisVoxelSystem = args->thisVoxelSystem;
        thisVoxelSystem->_voxelsUpdated += thisVoxelSystem->forceRemoveNodeFromArrays(voxel);
        thisVoxelSystem->setupNewVoxelsForDrawingSingleNode();
    }

    return true;
}

bool VoxelSystem::showAllSubTreeOperation(OctreeElement* element, void* extraData) {
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;
    hideOutOfViewArgs* args = (hideOutOfViewArgs*)extraData;
    
    // If we've culled at least once, then we will use the status of this voxel in the last culled frustum to determine
    // how to proceed. If we've never culled, then we just consider all these voxels to be UNKNOWN so that we will not
    // consider that case.
    if (args->culledOnce && args->wantDeltaFrustums) {
        ViewFrustum::location inLastCulledFrustum = voxel->inFrustum(args->lastViewFrustum);

        // if this node is fully inside our last culled view frustum, then we don't need to recurse further
        if (inLastCulledFrustum == ViewFrustum::INSIDE) {
            args->nodesInsideInside++;
            return false;
        }
    }

    args->nodesInside++;

    float voxelSizeScale = Menu::getInstance()->getVoxelSizeScale();
    int boundaryLevelAdjust = Menu::getInstance()->getBoundaryLevelAdjust();
    bool shouldRender = voxel->calculateShouldRender(&args->thisViewFrustum, voxelSizeScale, boundaryLevelAdjust);
    voxel->setShouldRender(shouldRender);

    if (shouldRender && !voxel->isKnownBufferIndex()) {
        // These are both needed to force redraw...
        voxel->setDirtyBit();
        voxel->markWithChangedTime();
        args->nodesShown++;
    }

    return true; // keep recursing!
}

// "hide" voxels in the VBOs that are still in the tree that but not in view.
// We don't remove them from the tree, we don't delete them, we do remove them
// from the VBOs and mark them as such in the tree.
bool VoxelSystem::hideOutOfViewOperation(OctreeElement* element, void* extraData) {
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;
    hideOutOfViewArgs* args = (hideOutOfViewArgs*)extraData;
    
    // If we're still recursing the tree using this operator, then we don't know if we're inside or outside...
    // so before we move forward we need to determine our frustum location
    ViewFrustum::location inFrustum = voxel->inFrustum(args->thisViewFrustum);

    // If we've culled at least once, then we will use the status of this voxel in the last culled frustum to determine
    // how to proceed. If we've never culled, then we just consider all these voxels to be UNKNOWN so that we will not
    // consider that case.
    ViewFrustum::location inLastCulledFrustum = ViewFrustum::OUTSIDE; // assume outside, but should get reset to actual value

    if (args->culledOnce && args->wantDeltaFrustums) {
        inLastCulledFrustum = voxel->inFrustum(args->lastViewFrustum);
    }

    // ok, now do some processing for this node...
    switch (inFrustum) {
        case ViewFrustum::OUTSIDE: {
            // If this node is outside the current view, then we might want to hide it... unless it was previously OUTSIDE,
            // if it was previously outside, then we can safely assume it's already hidden, and we can also safely assume
            // that all of it's children are outside both of our views, in which case we can just stop recursing...
            if (args->culledOnce && args->wantDeltaFrustums && inLastCulledFrustum == ViewFrustum::OUTSIDE) {
                args->nodesScanned++;
                args->nodesOutsideOutside++;
                return false; // stop recursing this branch!
            }

            // if this node is fully OUTSIDE the view, but previously intersected and/or was inside the last view, then
            // we need to hide it. Additionally we know that ALL of it's children are also fully OUTSIDE so we can recurse
            // the children and simply mark them as hidden
            args->tree->recurseElementWithOperation(voxel, hideAllSubTreeOperation, args );
            return false;

        } break;
        case ViewFrustum::INSIDE: {
            // If this node is INSIDE the current view, then we might want to show it... unless it was previously INSIDE,
            // if it was previously INSIDE, then we can safely assume it's already shown, and we can also safely assume
            // that all of it's children are INSIDE both of our views, in which case we can just stop recursing...
            if (args->culledOnce && args->wantDeltaFrustums && inLastCulledFrustum == ViewFrustum::INSIDE) {
                args->nodesScanned++;
                args->nodesInsideInside++;
                return false; // stop recursing this branch!
            }

            // if this node is fully INSIDE the view, but previously INTERSECTED and/or was OUTSIDE the last view, then
            // we need to show it. Additionally we know that ALL of it's children are also fully INSIDE so we can recurse
            // the children and simply mark them as visible (as appropriate based on LOD)
            args->tree->recurseElementWithOperation(voxel, showAllSubTreeOperation, args);
            return false;
        } break;
        case ViewFrustum::INTERSECT: {
            args->nodesScanned++;
            // If this node INTERSECTS the current view, then we might want to show it... unless it was previously INSIDE
            // the last known view, in which case it will already be visible, and we know that all it's children are also
            // previously INSIDE and visible. So in this case stop recursing
            if (args->culledOnce && args->wantDeltaFrustums && inLastCulledFrustum == ViewFrustum::INSIDE) {
                args->nodesIntersectInside++;
                return false; // stop recursing this branch!
            }

            args->nodesIntersect++;

            // if the child node INTERSECTs the view, then we want to check to see if it thinks it should render
            // if it should render but is missing it's VBO index, then we want to flip it on, and we can stop recursing from
            // here because we know will block any children anyway
            
            float voxelSizeScale = Menu::getInstance()->getVoxelSizeScale();
            int boundaryLevelAdjust = Menu::getInstance()->getBoundaryLevelAdjust();
            bool shouldRender = voxel->calculateShouldRender(&args->thisViewFrustum, voxelSizeScale, boundaryLevelAdjust);
            voxel->setShouldRender(shouldRender);
            
            if (voxel->getShouldRender() && !voxel->isKnownBufferIndex()) {
                voxel->setDirtyBit(); // will this make it draw?
                voxel->markWithChangedTime(); // both are needed to force redraw
                args->nodesShown++;
                return false;
            }

            // If it INTERSECTS but shouldn't be displayed, then it's probably a parent and it is at least partially in view.
            // So we DO want to recurse the children because some of them may not be in view... nothing specifically to do,
            // just keep iterating the children
            return true;

        } break;
    } // switch

    return true; // keep going!
}


void VoxelSystem::nodeAdded(SharedNodePointer node) {
    if (node->getType() == NodeType::VoxelServer) {
        qDebug("VoxelSystem... voxel server %s added...", node->getUUID().toString().toLocal8Bit().constData());
        _voxelServerCount++;
    }
}

bool VoxelSystem::killSourceVoxelsOperation(OctreeElement* element, void* extraData) {
    VoxelTreeElement* voxel = (VoxelTreeElement*)element;
    QUuid killedNodeID = *(QUuid*)extraData;
    for (int i = 0; i < NUMBER_OF_CHILDREN; i++) {
        VoxelTreeElement* childNode = voxel->getChildAtIndex(i);
        if (childNode) {
            if (childNode->matchesSourceUUID(killedNodeID)) {
                voxel->safeDeepDeleteChildAtIndex(i);
            }
        }
    }
    return true;
}

void VoxelSystem::nodeKilled(SharedNodePointer node) {
    if (node->getType() == NodeType::VoxelServer) {
        _voxelServerCount--;
        QUuid nodeUUID = node->getUUID();
        qDebug("VoxelSystem... voxel server %s removed...", nodeUUID.toString().toLocal8Bit().constData());
    }
}

unsigned long VoxelSystem::getFreeMemoryGPU() {
    // We can't ask all GPUs how much memory they have in use, but we can ask them about how much is free.
    // So, we can record the free memory before we create our VBOs and the free memory after, and get a basic
    // idea how how much we're using.

    _hasMemoryUsageGPU = false; // assume the worst
    unsigned long freeMemory = 0;
    const int NUM_RESULTS = 4; // see notes, these APIs return up to 4 results
    GLint results[NUM_RESULTS] = { 0, 0, 0, 0};

    // ATI
    // http://www.opengl.org/registry/specs/ATI/meminfo.txt
    //
    // TEXTURE_FREE_MEMORY_ATI                 0x87FC
    // RENDERBUFFER_FREE_MEMORY_ATI            0x87FD
    const GLenum VBO_FREE_MEMORY_ATI = 0x87FB;
    glGetIntegerv(VBO_FREE_MEMORY_ATI, &results[0]);
    GLenum errorATI = glGetError();

    if (errorATI == GL_NO_ERROR) {
        _hasMemoryUsageGPU = true;
        freeMemory = results[0];
    } else {

        // NVIDIA
        // http://developer.download.nvidia.com/opengl/specs/GL_NVX_gpu_memory_info.txt
        //
        //const GLenum GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX = 0x9047;
        //const GLenum GPU_MEMORY_INFO_EVICTION_COUNT_NVX = 0x904A;
        //const GLenum GPU_MEMORY_INFO_EVICTED_MEMORY_NVX = 0x904B;
        //const GLenum GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX = 0x9048;

        const GLenum GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX = 0x9049;
        results[0] = 0;
        glGetIntegerv(GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &results[0]);
        freeMemory += results[0];
        GLenum errorNVIDIA = glGetError();

        if (errorNVIDIA == GL_NO_ERROR) {
            _hasMemoryUsageGPU = true;
            freeMemory = results[0];
        }
    }

    const unsigned long BYTES_PER_KBYTE = 1024;
    return freeMemory * BYTES_PER_KBYTE; // API results in KB, we want it in bytes
}

unsigned long VoxelSystem::getVoxelMemoryUsageGPU() {
    unsigned long currentFreeMemory = getFreeMemoryGPU();
    return (_initialMemoryUsageGPU - currentFreeMemory);
}

void VoxelSystem::bindPerlinModulateProgram() {
    if (!_perlinModulateProgram.isLinked()) {
        _perlinModulateProgram.addShaderFromSourceFile(QGLShader::Vertex,
            Application::resourcesPath() + "shaders/perlin_modulate.vert");
        _perlinModulateProgram.addShaderFromSourceFile(QGLShader::Fragment,
            Application::resourcesPath() + "shaders/perlin_modulate.frag");
        _perlinModulateProgram.link();

        _perlinModulateProgram.bind();
        _perlinModulateProgram.setUniformValue("permutationNormalTexture", 0);
    
    } else {
        _perlinModulateProgram.bind();
    }
}

void VoxelSystem::createCube() {
    if (_voxelModelID == 0) {
        GLfloat vertexPos[] = {
            0.0f, 0.0f, 1.0f, 1.0f,
            1.0f, 0.0f, 1.0f, 1.0f,
            0.0f, 1.0f, 1.0f, 1.0f,
            1.0f, 1.0f, 1.0f, 1.0f,
            
            0.0f, 1.0f, 0.0f, 1.0f,
            1.0f, 1.0f, 0.0f, 1.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            1.0f, 0.0f, 0.0f, 1.0f,
        };
        
        GLfloat normals[] = {
            1.0f, 0.0f, 0.0f, 1.0f,     // 0
            -1.0f, 0.0f, 0.0f, 1.0f,    // 1
            0.0f, 1.0f, 0.0f, 1.0f,     // 2
            0.0f, -1.0f, 0.0f, 1.0f,    // 3
            0.0f, 0.0f, 1.0f, 1.0f,     // 4
            0.0f, 0.0f, -1.0f, 1.0f,    // 5
        };
        
        GLfloat uvs[] = {
            0.0f, 0.0f,                 // 0
            0.0f, 0.0f,                 // 1
            0.0f, 0.0f,                 // 2
            0.0f, 0.0f,                 // 3
            
            0.0f, 0.0f,                 // 4
            0.0f, 0.0f,                 // 5
            0.0f, 0.0f,                 // 6
            0.0f, 0.0f,                 // 7
        };
        
        GLuint posIndices[] = {
            // front
            0, 1, 2,
            2, 1, 3,
            
            // top
            2, 3, 4,
            4, 3, 5,
            
            // back
            4, 5, 6,
            6, 5, 7,
            
            // bottom
            6, 7, 0,
            0, 7, 1,
            
            // right
            1, 7, 3,
            3, 7, 5,
            
            // left
            6, 0, 4,
            4, 0, 2,
        };
        
        GLuint normalIndices[] = {
            // front
            5, 5, 5,
            5, 5, 5,
            
            // top
            2, 2, 2,
            2, 2, 2,
            
            // back
            4, 4, 4,
            4, 4, 4,
            
            // bottom
            3, 3, 3,
            3, 3, 3,
            
            // right
            0, 0, 0,
            0, 0, 0,
            
            // left
            1, 1, 1,
            1, 1, 1,
        };
        
        GLuint uvIndices[] = {
            // front
            0, 0, 0,
            0, 0, 0,
            
            // back
            0, 0, 0,
            0, 0, 0,
            
            // top
            0, 0, 0,
            0, 0, 0,
            
            // bottom
            0, 0, 0,
            0, 0, 0,
            
            // left
            0, 0, 0,
            0, 0, 0,
            
            // right
            0, 0, 0,
            0, 0, 0,
        };
        
        int numVertices = sizeof(vertexPos) / sizeof(*vertexPos);
        numVertices /= 4;                           // x, y, z, w
        
        GLuint* posIndicesPtr = posIndices;
        GLuint* normalIndicesPtr = normalIndices;
        GLuint* uvIndicesPtr = uvIndices;
        
        // number of vertices for all the faces
        int numFaceVertices = sizeof(posIndices) / sizeof(*posIndices);
        
        // number of unique vertices with (position, normal, uv)
        int totalFloatVals = numFaceVertices * 10;
        
        // vbo data
        GLfloat* vboData = new GLfloat[totalFloatVals];
        GLfloat* vboDataPtr = vboData;
        
        int count = 0;
        for (int i = 0; i < numFaceVertices; i++) {
            int posIndex = (*posIndicesPtr++) * 4;
            int normalIndex = (*normalIndicesPtr++) * 4;
            int uvIndex = (*uvIndicesPtr++) * 2;
            
            // position
            *vboDataPtr++ = vertexPos[posIndex];
            *vboDataPtr++ = vertexPos[posIndex+1];
            *vboDataPtr++ = vertexPos[posIndex+2];
            *vboDataPtr++ = vertexPos[posIndex+3];
            
            count += 4;
            
            // normal
            *vboDataPtr++ = normals[normalIndex];
            *vboDataPtr++ = normals[normalIndex+1];
            *vboDataPtr++ = normals[normalIndex+2];
            *vboDataPtr++ = normals[normalIndex+3];
            
            count += 4;
            
            // uv
            *vboDataPtr++ = uvs[uvIndex];
            *vboDataPtr++ = uvs[uvIndex+1];
            
            count += 2;
        }
        
        assert(count == totalFloatVals);
        
        // indices
        GLuint* vboIndices = new GLuint[numFaceVertices];
        GLuint* vboIndicesPtr = vboIndices;
        for (int i = 0; i < numFaceVertices; i++) {
            *vboIndicesPtr++ = i;
        }
    
        // model vbo
        glGenBuffers(1, &_voxelModelID);
        glBindBuffer(GL_ARRAY_BUFFER, _voxelModelID);
        glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * totalFloatVals, vboData, GL_STATIC_DRAW);
        
        glGenBuffers(1, &_voxelModelIndicesID);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _voxelModelIndicesID);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLuint) * numFaceVertices, vboIndices, GL_STATIC_DRAW);
        
        int strideSize = sizeof(GLfloat) * 4;
        
        // position
        glVertexAttribPointer(_positionShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, strideSize, 0);
        glEnableVertexAttribArray(_positionShaderAttributeLocation);
        
        // normal
        glVertexAttribPointer(_normalShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, strideSize, (void *)(sizeof(float) * 4));
        glEnableVertexAttribArray(_normalShaderAttributeLocation);
        
        // uv
        glVertexAttribPointer(_uvShaderAttributeLocation, 4, GL_FLOAT, GL_FALSE, strideSize, (void *)(sizeof(float) * 4 * 2));
        glEnableVertexAttribArray(_uvShaderAttributeLocation);
        
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        
        delete[] vboIndices;
        delete[] vboData;
        
        _memoryUsageVBO += sizeof(GLfloat) * numVertices;
        _memoryUsageVBO += sizeof(GLuint) * numVertices;
    }
}

// Swizzle value of bit pairs of the value of index
unsigned short VoxelSystem::_sSwizzledOcclusionBits[64] = {
    0x0000, // 00000000
    0x0002, // 00000001
    0x0001, // 00000010
    0x0003, // 00000011
    0x0008, // 00000100
    0x000a, // 00000101
    0x0009, // 00000110
    0x000b, // 00000111
    0x0004, // 00001000
    0x0006, // 00001001
    0x0005, // 00001010
    0x0007, // 00001011
    0x000c, // 00001100
    0x000e, // 00001101
    0x000d, // 00001110
    0x000f, // 00001111
    0x0020, // 00010000
    0x0022, // 00010001
    0x0021, // 00010010
    0x0023, // 00010011
    0x0028, // 00010100
    0x002a, // 00010101
    0x0029, // 00010110
    0x002b, // 00010111
    0x0024, // 00011000
    0x0026, // 00011001
    0x0025, // 00011010
    0x0027, // 00011011
    0x002c, // 00011100
    0x002e, // 00011101
    0x002d, // 00011110
    0x002f, // 00011111
    0x0010, // 00100000
    0x0012, // 00100001
    0x0011, // 00100010
    0x0013, // 00100011
    0x0018, // 00100100
    0x001a, // 00100101
    0x0019, // 00100110
    0x001b, // 00100111
    0x0014, // 00101000
    0x0016, // 00101001
    0x0015, // 00101010
    0x0017, // 00101011
    0x001c, // 00101100
    0x001e, // 00101101
    0x001d, // 00101110
    0x001f, // 00101111
    0x0030, // 00110000
    0x0032, // 00110001
    0x0031, // 00110010
    0x0033, // 00110011
    0x0038, // 00110100
    0x003a, // 00110101
    0x0039, // 00110110
    0x003b, // 00110111
    0x0034, // 00111000
    0x0036, // 00111001
    0x0035, // 00111010
    0x0037, // 00111011
    0x003c, // 00111100
    0x003e, // 00111101
    0x003d, // 00111110
    0x003f, // 00111111
};

// Octant bitmask array indexed by octant. The mask value indicates the octant's halfspace partitioning. The index
// value corresponds to the voxel's octal code derived in "pointToVoxel" in SharedUtil.cpp, which, BTW, does *not*
// correspond to the "ChildIndex" enum value in OctreeElement.h
unsigned char VoxelSystem::_sOctantIndexToBitMask[8] = { 
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Left  | OctreeElement::HalfSpace::Near,
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Left  | OctreeElement::HalfSpace::Far,
        OctreeElement::HalfSpace::Top    | OctreeElement::HalfSpace::Left  | OctreeElement::HalfSpace::Near,
        OctreeElement::HalfSpace::Top    | OctreeElement::HalfSpace::Left  | OctreeElement::HalfSpace::Far,
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Right | OctreeElement::HalfSpace::Near,
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Right | OctreeElement::HalfSpace::Far,
        OctreeElement::HalfSpace::Top    | OctreeElement::HalfSpace::Right | OctreeElement::HalfSpace::Near,
        OctreeElement::HalfSpace::Top    | OctreeElement::HalfSpace::Right | OctreeElement::HalfSpace::Far,
};

// Two dimensional array map indexed by octant row and column. The mask value
// indicates the two faces shared by the octants
unsigned char VoxelSystem::_sOctantIndexToSharedBitMask[8][8] = {
    { // Index 0: Bottom-Left-Near
        0,    // Bottom-Left-Near
        OctreeElement::HalfSpace::Near   | OctreeElement::HalfSpace::Far,    // Bottom-Left-Far
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Top,    // Top-Left-Near
        0,    // Top-Left-Far
        OctreeElement::HalfSpace::Right  | OctreeElement::HalfSpace::Left,    // Bottom-Right-Near
        0,    // Bottom-Right-Far
        0,    // Top-Right-Near
        0,    // Top-Right-Far
    },
    { // Index 1: Bottom-Left-Far
        OctreeElement::HalfSpace::Near   | OctreeElement::HalfSpace::Far,    // Bottom-Left-Near
        0,    // Bottom-Left-Far
        0,    // Top-Left-Near
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Top,    // Top-Left-Far
        0,    // Bottom-Right-Near
        OctreeElement::HalfSpace::Right  | OctreeElement::HalfSpace::Left,    // Bottom-Right-Far
        0,    // Top-Right-Near
        0,    // Top-Right-Far
    },
    { // Index 2: Top-Left-Near
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Top,    // Bottom-Left-Near
        0,    // Bottom-Left-Far
        0,    // Top-Left-Near
        OctreeElement::HalfSpace::Near   | OctreeElement::HalfSpace::Far,    // Top-Left-Far
        0,    // Bottom-Right-Near
        0,    // Bottom-Right-Far
        OctreeElement::HalfSpace::Right  | OctreeElement::HalfSpace::Left,    // Top-Right-Near
        0,    // Top-Right-Far
    },
    { // Index 3: Top-Left-Far
        0,    // Bottom-Left-Near
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Top,    // Bottom-Left-Far
        OctreeElement::HalfSpace::Near   | OctreeElement::HalfSpace::Far,    // Top-Left-Near
        0,    // Top-Left-Far
        0,    // Bottom-Right-Near
        0,    // Bottom-Right-Far
        0,    // Top-Right-Near
        OctreeElement::HalfSpace::Right  | OctreeElement::HalfSpace::Left,    // Top-Right-Far
    },
    { // Index 4: Bottom-Right-Near
        OctreeElement::HalfSpace::Right  | OctreeElement::HalfSpace::Left,    // Bottom-Left-Near
        0,    // Bottom-Left-Far
        0,    // Top-Left-Near
        0,    // Top-Left-Far
        0,    // Bottom-Right-Near
        OctreeElement::HalfSpace::Near   | OctreeElement::HalfSpace::Far,    // Bottom-Right-Far
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Top,    // Top-Right-Near
        0,    // Top-Right-Far
    },
    { // Index 5: Bottom-Right-Far
        0,    // Bottom-Left-Near
        OctreeElement::HalfSpace::Right  | OctreeElement::HalfSpace::Left,    // Bottom-Left-Far
        0,    // Top-Left-Near
        0,    // Top-Left-Far
        OctreeElement::HalfSpace::Near   | OctreeElement::HalfSpace::Far,    // Bottom-Right-Near
        0,    // Bottom-Right-Far
        0,    // Top-Right-Near
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Top,    // Top-Right-Far
    },
    { // Index 6: Top-Right-Near
        0,    // Bottom-Left-Near
        0,    // Bottom-Left-Far
        OctreeElement::HalfSpace::Right  | OctreeElement::HalfSpace::Left,    // Top-Left-Near
        0,    // Top-Left-Far
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Top,    // Bottom-Right-Near
        0,    // Bottom-Right-Far
        0,    // Top-Right-Near
        OctreeElement::HalfSpace::Near   | OctreeElement::HalfSpace::Far,    // Top-Right-Far
    },
    { // Index 7: Top-Right-Far
        0,    // Bottom-Left-Near
        0,    // Bottom-Left-Far
        0,    // Top-Left-Near
        OctreeElement::HalfSpace::Right  | OctreeElement::HalfSpace::Left,    // Top-Left-Far
        0,    // Bottom-Right-Near
        OctreeElement::HalfSpace::Bottom | OctreeElement::HalfSpace::Top,    // Bottom-Right-Far
        OctreeElement::HalfSpace::Near   | OctreeElement::HalfSpace::Far,    // Top-Right-Near
        0,    // Top-Right-Far
    },
};


