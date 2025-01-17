/*
Copyright (c) 2013 Benedikt Bitterli

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/


#include "ThreadBarrier.hpp"
#include "VoxelOctree.hpp"
#include "PlyLoader.hpp"
#include "VoxelData.hpp"
#include "Events.hpp"
#include "Timer.hpp"
#include "Util.hpp"

#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"

#include "math/MatrixStack.hpp"
#include "math/Vec3.hpp"
#include "math/Mat4.hpp"

#include <algorithm>
#include <stdlib.h>
#include <iostream>
#include <stdio.h>
#include <cstring>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <cmath>
#include <SDL.h>
#include <SDL/SDL_image.h>

#ifndef M_PI
#define M_PI        3.14159265358979323846
#endif
#define ITER 12
#define SEED 2
#define MAXBOUNCES 8
#define ROUGHNESS 2
#define INV255 0.00392156862
static const double INVITER = 1.0f/ITER;
static const float nScale = 1.f/1024.f;
/* Number of threads to use - adapt this to your platform for optimal results */
static const int NumThreads = 2;
/* Screen resolution */
static const int GWidth  = 1024;
static const int GHeight = 768;

static const float AspectRatio = GHeight/(float)GWidth;
static const int TileSize = 16;

static SDL_Surface *backBuffer;
Uint32 *hdrpixels;
static ThreadBarrier *barrier;

static std::atomic<bool> doTerminate;
static std::atomic<bool> renderHalfSize;

int hdrw;
int hdrh;
int hdrpitch;

struct BatchData {
    int id;
    int x0, y0;
    int x1, y1;

    int tilesX, tilesY;
    float *depthBuffer;
    VoxelOctree *tree;
};

float vectorAngle(float x, float y) {
    if (!x) // special cases
        return (y > 0)? 0.25
            : (!y)? 0.f
            : 0.75f;
    else if (!y) // special cases
        return (x >= 0)? 0.f
            : 0.5f;
    float ret = atanf(y/x) / M_PI;
    if (x < 0 && y < 0) // quadrant Ⅲ
        ret += 0.5f;
    else if (x < 0) // quadrant Ⅱ
        ret -= 0.5f; // it actually substracts
    else if (y < 0) // quadrant Ⅳ
        ret = 0.75f + (0.25 - ret); // it actually substracts
    return ret;
}

Vec3 light = Vec3(-1.0, 1.0, -1.0).normalize();

float hdri(Vec3 v) {
	float vn = std::max(0.05f, light.dot(v.normalize()));
	return vn;
	/*int x = (int)(vectorAngle(vn.x,vn.y)*hdrw);
	int y = (int)(hdrh*0.5*(vn.z+1));
	int colour = hdrpixels[y*hdrh+x];
	return Vec3((colour & 0xFF) * INV255, ((colour >> 8) & 0xFF) * INV255, ((colour >> 16) & 0xFF)*INV255);*/
	//img location from -1 to 1 and return pixel of image
}

float falloff() {
	float n = (rand()%1000)*0.001f;
	return n*n; //square falloff
}

Vec3 randomVector() {
		float z = -1.0f + static_cast <float> (rand()) /( static_cast <float> (RAND_MAX/(2.f)));
		float v = static_cast <float> (rand()) /( static_cast <float> (RAND_MAX/(6.4258530716)));
		float x = sin(v) * -cos(asin(z));
		float y = cos(v) * z;
		return Vec3(x,y,z);
}

/*Vec3 randomUnitVector() {
	return randomVector().normalize();
}*/
Vec3 shade(const Vec3 pos, float t, int intNormal, const Vec3 &ray, VoxelOctree *tree) {
    uint32 intNormal2;
	float t2;
	
    Vec3 n;
    float c;
    decompressMaterial(intNormal, n, c);
    Vec3 absorption = Vec3(0.6,0.6,0.6);
   
	Vec3 pos2 = pos + ray*t + nScale*n; 
	Vec3 reflRay = ray - 2*n.dot(ray)*n;
	Vec3 tcol = Vec3(0);
	for(int i = 0; i < ITER; i++) {
		Vec3 col = absorption;
		int bounces = 0;
		again:;
		Vec3 randRay = reflRay + randomVector()*ROUGHNESS*falloff();
		randRay -= (randRay.dot(n) > 0) ? 0: 2*n.dot(randRay)*n;
		if(tree->raymarch(pos2, randRay, 0.0f, intNormal2, t2)) {
			if(bounces < MAXBOUNCES) {
				bounces++;
				col*=absorption;
				decompressMaterial(intNormal2, n, c);
				pos2 += randRay*t2 + nScale*n;
				reflRay = randRay - 2*n.dot(randRay)*n;
				goto again;
			}
			else {
				col = Vec3(0,0,0);
			}
		}
		else {
			col*=hdri(randRay);
		}
		tcol+=col;
	}
	tcol *= INVITER;
    return tcol;
}

void renderTile(int x0, int y0, int x1, int y1, int stride, float scale, float zx, float zy, float zz,
        const Mat4 &tform,  VoxelOctree *tree, const Vec3 &pos, float minT) {
    uint32 *buffer  = (uint32 *)backBuffer->pixels;
    int pitch       = backBuffer->pitch / 4;

    float dy = AspectRatio - y0*scale;
    for (int y = y0; y < y1; ++y, dy -= scale) {
        float dx = -1.0f + x0*scale;
        for (int x = x0; x < x1; ++x, dx += scale) {
            int cornerX = x - ((x - x0) % stride);
            int cornerY = y - ((y - y0) % stride);
            if (cornerX != x || cornerY != y) {
                buffer[x + y*pitch] = buffer[cornerX + cornerY*pitch];
                continue;
            }

            Vec3 dir = Vec3(
                dx*tform.a11 + dy*tform.a12 + zx,
                dx*tform.a21 + dy*tform.a22 + zy,
                dx*tform.a31 + dy*tform.a32 + zz
            );
            dir *= invSqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);

            uint32 intNormal;
            float t;
            Vec3 col;
            if (tree->raymarch(pos + dir*minT, dir, 0.0f, intNormal, t)) {
                col = shade(pos,(minT+t), intNormal, dir,tree);
            }

#ifdef __APPLE__
            uint32 color =
                 uint32(std::min(col.x, 1.0f)*255.0) <<  8   |
                (uint32(std::min(col.y, 1.0f)*255.0) <<  16) |
                (uint32(std::min(col.z, 1.0f)*255.0) <<  24) |
                0x000000FFu;
#else
            uint32 color =
                 uint32(std::min(col.x, 1.0f)*255.0)        |
                (uint32(std::min(col.y, 1.0f)*255.0) <<  8) |
                (uint32(std::min(col.z, 1.0f)*255.0) << 16) |
                0xFF000000u;
#endif
            buffer[x + y*pitch] = color;
        }
    }
}

void renderBatch(BatchData *data) {
    const float TreeMiss = 1e10;

    int x0 = data->x0, y0 = data->y0;
    int x1 = data->x1, y1 = data->y1;
    int tilesX = data->tilesX;
    int tilesY = data->tilesY;
    float *depthBuffer = data->depthBuffer;
    VoxelOctree *tree = data->tree;

    Mat4 tform;
    MatrixStack::get(INV_MODELVIEW_STACK, tform);

    Vec3 pos = tform*Vec3() + tree->center() + Vec3(1.0);

    tform.a14 = tform.a24 = tform.a34 = 0.0f;

    float scale = 2.0f/GWidth;
    float tileScale = TileSize*scale;
    float planeDist = 1.0f/std::tan(float(M_PI)/6.0f);
    float zx = planeDist*tform.a13, zy = planeDist*tform.a23, zz = planeDist*tform.a33;
    float coarseScale = 2.0f*TileSize/(planeDist*GHeight);
    int stride = renderHalfSize ? 3 : 1;

    std::memset((uint8 *)backBuffer->pixels + y0*backBuffer->pitch, 0, (y1 - y0)*backBuffer->pitch);

    float dy = AspectRatio - y0*scale;
    for (int y = 0, idx = 0; y < tilesY; y++, dy -= tileScale) {
        float dx = -1.0f + x0*scale;
        for (int x = 0; x < tilesX; x++, dx += tileScale, idx++) {
            Vec3 dir = Vec3(
                dx*tform.a11 + dy*tform.a12 + zx,
                dx*tform.a21 + dy*tform.a22 + zy,
                dx*tform.a31 + dy*tform.a32 + zz
            );
            dir *= invSqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);

            uint32 intNormal;
            float t;
            Vec3 col;
            if (tree->raymarch(pos, dir, coarseScale, intNormal, t))
                depthBuffer[idx] = t;
            else
                depthBuffer[idx] = TreeMiss;

            if (x > 0 && y > 0) {
                float minT = std::min(std::min(depthBuffer[idx], depthBuffer[idx - 1]),
                    std::min(depthBuffer[idx - tilesX],
                    depthBuffer[idx - tilesX - 1]));

                if (minT != TreeMiss) {
                    int tx0 = (x - 1)*TileSize + x0;
                    int ty0 = (y - 1)*TileSize + y0;
                    int tx1 = std::min(tx0 + TileSize, x1);
                    int ty1 = std::min(ty0 + TileSize, y1);
                    renderTile(tx0, ty0, tx1, ty1, stride, scale, zx, zy, zz, tform, tree, pos,
                            std::max(minT - 0.03f, 0.0f));
                }
            }
        }
    }
}

int renderLoop(void *threadData) {
    BatchData *data = (BatchData *)threadData;
	renderHalfSize = false;
    float radius = 1.3f;
    /*float pitch = 0.0f;
    float yaw = 0.0f;*/

    if (data->id == 0) {
        MatrixStack::set(VIEW_STACK, Mat4::translate(Vec3(0.0f, 0.0f, -radius)));
        MatrixStack::set(MODEL_STACK, Mat4());
    }
    barrier->waitPre();
    Timer timer;
    renderBatch(data);
    timer.bench("Render time was ");
    barrier->waitPost();
    
    while (!doTerminate) {

        if (data->id == 0) {
            if (SDL_MUSTLOCK(backBuffer))
                SDL_UnlockSurface(backBuffer);

            SDL_UpdateRect(backBuffer, 0, 0, 0, 0);

            int event;
            while ((event = waitEvent()) && (event == SDL_MOUSEMOTION && !getMouseDown(0) && !getMouseDown(1)));

            if (getKeyDown(SDLK_ESCAPE)) {
                doTerminate = true;
                barrier->releaseAll();
            }

            /*float mx = float(getMouseXSpeed());
            float my = float(getMouseYSpeed());
            if (getMouseDown(0) && (mx != 0 || my != 0)) {
                pitch = std::fmod(pitch - my, 360.0f);
                yaw = std::fmod(yaw + (std::fabs(pitch) > 90.0f ? mx : -mx), 360.0f);

                     if (pitch >  180.0f) pitch -= 360.0f;
                else if (pitch < -180.0f) pitch += 360.0f;

                MatrixStack::set(MODEL_STACK, Mat4::rotXYZ(Vec3(pitch, 0.0f, 0.0f))*
                        Mat4::rotXYZ(Vec3(0.0f, yaw, 0.0f)));
                renderHalfSize = true;
            } else if (getMouseDown(1) && my != 0) {
                radius *= std::min(std::max(1.0f - my*0.01f, 0.5f), 1.5f);
                radius = std::min(radius, 25.0f);
                MatrixStack::set(VIEW_STACK, Mat4::translate(Vec3(0.0f, 0.0f, -radius)));
                renderHalfSize = true;
            } else {
                renderHalfSize = false;
            }*/
            //if (SDL_MUSTLOCK(backBuffer))
            //    SDL_LockSurface(backBuffer);
        }
    }
	SDL_SaveBMP(backBuffer, "./screenshot.bmp");
    return 0;
}

/* Maximum allowed memory allocation sizes for lookup table and cache blocks.
 * Larger => faster conversion usually, but adapt this to your own RAM size.
 * The conversion will still succeed with memory sizes much, much smaller than
 * the size of the voxel data, only slower.
 */
static const size_t dataMemory = int64_t(1024)*1024*1024;

void printHelp() {
    std::cout << "Usage: sparse-voxel-octrees [options] filename ..." << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "-builder              set program to SVO building mode." << std::endl;
    std::cout << "  --resolution <r>    set voxel resolution. r is an integer which equals to a power of 2." << std::endl;
    std::cout << "  --mode <m>          set where to generate voxel data, m equals 0 or 1, where 0 indicates GENERATE_IN_MEMORY while 1 indicates GENERATE_ON_DISK." << std::endl;
    std::cout << "-viewer               set program to SVO rendering mode." << std::endl << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  sparse-voxel-octrees -builder --resolution 256 --mode 0 ../models/xyzrgb_dragon.ply ../models/xyzrgb_dragon.oct" << std::endl;
    std::cout << "  sparse-voxel-octrees -builder ../models/xyzrgb_dragon.ply ../models/xyzrgb_dragon.oct" << std::endl;
    std::cout << "  sparse-voxel-octrees -viewer ../models/XYZRGB-Dragon.oct" << std::endl << std::endl << std::endl;
}

int main(int argc, char *argv[]) {
    
    for(int s = 0; s < SEED; s++) {
    	rand();
    }
    unsigned int resolution = 256;  //default resolution
    unsigned int mode = 0;          //default to generate in memory
    std::string inputFile = "";
    std::string outputFile = "";
    
    /* parse arguments */
    if ((argc == 8) && (std::string(argv[1]) == "-builder")) {
        resolution = atoi(argv[3]);
        mode = atoi(argv[5]);
        inputFile = argv[6];
        outputFile = argv[7];
    }
    else if ((argc == 4) && (std::string(argv[1]) == "-builder")) {
        inputFile = argv[2];
        outputFile = argv[3];
    }
    else if ((argc == 3) && (std::string(argv[1]) == "-viewer")) 
        inputFile = argv[2];
    else {
        std::cout << "Invalid arguments! Please refer to the help info!" << std::endl;
        printHelp();
        return 0;
    }

    Timer timer;
    
    if (std::string(argv[1]) == "-builder") {
        ThreadUtils::startThreads(ThreadUtils::idealThreadCount());

        if (mode) { //generate on disk
            std::unique_ptr<PlyLoader> loader(new PlyLoader(inputFile.c_str()));
            loader->convertToVolume("models/temp.voxel", resolution, dataMemory);
            std::unique_ptr<VoxelData> data(new VoxelData("models/temp.voxel", dataMemory));
            std::unique_ptr<VoxelOctree> tree(new VoxelOctree(data.get()));
            tree->save(outputFile.c_str());
        } 
        else {      //generate in memory
            std::unique_ptr<PlyLoader> loader(new PlyLoader(inputFile.c_str()));
            std::unique_ptr<VoxelData> data(new VoxelData(loader.get(), resolution, dataMemory));
            std::unique_ptr<VoxelOctree> tree(new VoxelOctree(data.get()));
            tree->save(outputFile.c_str());
        }
        timer.bench("Octree initialization took");
        return 0;
    }

    if (std::string(argv[1]) == "-viewer")  {
        std::unique_ptr<VoxelOctree> tree(new VoxelOctree(inputFile.c_str()));

        timer.bench("Octree initialization took");

        SDL_Init(SDL_INIT_VIDEO);

        SDL_WM_SetCaption("Sparse Voxel Octrees", "Sparse Voxel Octrees");
        backBuffer = SDL_SetVideoMode(GWidth, GHeight, 32, SDL_SWSURFACE);
		SDL_Surface *hdr = SDL_LoadBMP( "hdri.bmp" );
		hdrw = hdr->w;
		hdrh = hdr->h;
		hdrpitch = hdr->pitch;
		hdrpixels = (Uint32*)hdr->pixels;
        SDL_Thread *threads[NumThreads - 1];
        BatchData threadData[NumThreads];

        barrier = new ThreadBarrier(NumThreads);
        doTerminate = false;

        if (SDL_MUSTLOCK(backBuffer))
            SDL_LockSurface(backBuffer);

        int stride = (GHeight - 1) / NumThreads + 1;
        for (int i = 0; i < NumThreads; i++) {
            threadData[i].id = i;
            threadData[i].tree = tree.get();
            threadData[i].x0 = 0;
            threadData[i].x1 = GWidth;
            threadData[i].y0 = i*stride;
            threadData[i].y1 = std::min((i + 1)*stride, GHeight);
            threadData[i].tilesX = (threadData[i].x1 - threadData[i].x0 - 1) / TileSize + 2;
            threadData[i].tilesY = (threadData[i].y1 - threadData[i].y0 - 1) / TileSize + 2;
            threadData[i].depthBuffer = new float[threadData[i].tilesX*threadData[i].tilesY];
        }

        for (int i = 1; i < NumThreads; i++)
            threads[i - 1] = SDL_CreateThread(&renderLoop, (void *)&threadData[i]);
        renderLoop((void *)&threadData[0]);

        for (int i = 1; i < NumThreads; i++)
            SDL_WaitThread(threads[i - 1], 0);

        if (SDL_MUSTLOCK(backBuffer))
            SDL_UnlockSurface(backBuffer);

        SDL_Quit();
    }

    return 0;
}
