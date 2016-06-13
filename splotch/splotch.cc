/*
 * Copyright (c) 2004-2014
 *              Martin Reinecke (1), Klaus Dolag (1)
 *               (1) Max-Planck-Institute for Astrophysics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <iostream>
#include <cmath>
#include <cstdio>
#include <algorithm>

#ifdef SPLVISIVO
#include "optionssetter.h"
#endif

#include "splotch/scenemaker.h"
#include "splotch/splotchutils.h"
#include "splotch/splotch_host.h"
#include "cxxsupport/walltimer.h"
#include "cxxsupport/ls_image.h"
#include "cxxsupport/announce.h"

#ifdef CUDA
#include "cuda/cuda_splotch.h"
#include <thread>
#endif
#ifdef OPENCL
#include "opencl/splotch_cuda2.h"
#endif

#ifdef MIC
#include "mic/mic_splotch.h"
#endif

#ifdef PREVIEWER
#include "previewer/simple_gui/SimpleGUI.h"
#include "previewer/libs/core/FileLib.h"
#include <string>
#endif

using namespace std;
MPI_Request req;
bool first=false;
#ifdef SPLVISIVO
int splotchMain (VisIVOServerOptions opt)
#else
int main (int argc, const char **argv)
#endif
  {

#ifdef PREVIEWER
  bool pvMode=false;
  int paramFileArg=-1;
  for (int i=0; i<argc; i++)
    {
    // Look for command line switch (-pv launches previewer)
    if (string(argv[i]) == string("-pv"))
      pvMode = true;

    // Look for the parameter file
    if (previewer::FileLib::FileExists(argv[i]))
      paramFileArg = i;
    }
  // Preview mode is enabled
  if (pvMode)
    {
    // If param file exists launch app
    if (paramFileArg>0)
      {
      // Launch app with simple GUI
      previewer::simple_gui::SimpleGUI program;
      program.Load(string(argv[paramFileArg]));
      }
    else
      {
      // Output a message as input param file does not exist
      cout << "Invalid input parameter file." << endl;
      return 0;
      }
    }
#endif

  tstack_push("Splotch");
  tstack_push("Setup");
  bool master = mpiMgr.master();

  // Prevent the rendering to quit if the "stop" file
  // has been forgotten before Splotch was started
  if (master)
    if (file_present("stop")) remove("stop");

#ifdef SPLVISIVO
  planck_assert(!opt.splotchpar.empty(),"usage: --splotch <parameter file>");
  paramfile params (opt.splotchpar.c_str(),true);
  params.setParam("camera_x",opt.spPosition[0]);
  params.setParam("camera_y",opt.spPosition[1]);
  params.setParam("camera_z",opt.spPosition[2]);
  params.setParam("lookat_x",opt.spLookat[0]);
  params.setParam("lookat_y",opt.spLookat[1]);
  params.setParam("lookat_z",opt.spLookat[2]);
  params.setParam("outfile",visivoOpt->imageName);
  params.setParam("interpolation_mode",0);
  params.setParam("simtype",10);
  params.setParam("fov",opt.spFov);
  params.setParam("ptypes",1);
#else
  module_startup ("splotch",argc,argv,2,"<parameter file>",master);
  paramfile params (argv[1],false);

  planck_assert(!params.param_present("geometry_file"),
    "Creating of animations has been changed, use \"scene_file\" instead of "
    "\"geometry_file\" ... ");
#endif

  vector<particle_sim> particle_data; //raw data from file
  vector<particle_sim> r_points;

// Many Integrated Core Initialisation
#ifdef MIC
  tstack_push("Init mic");
  // Init mic device with empty offload
  // (if not already done by environment variable)
  #pragma offload_transfer target(mic:0)

  #pragma offload target(mic:0)
  {
    // Initialise openmp threads
    #pragma omp parallel
    {

    }
  }
  tstack_pop("Init mic");
#endif

// CUDA/OPENCP Initialisation
#if (!defined(OPENCL))
  vec3 campos, centerpos, lookat, sky;
  vector<COLOURMAP> amap;
#else
  ptypes = params.find<int>("ptypes",1);
  g_params =&params;
#endif
#if (defined(CUDA) || defined(OPENCL))
  int myID = mpiMgr.rank();
  int nDevNode = check_device(myID);     // number of GPUs available per node
  int mydevID = -1;
  int nTasksDev;     // number of processes using the same GPU
#ifdef CUDA
  // We assume a geometry where processes use only one gpu if available
  int nDevProc = 1;   // number of GPU required per process
  int nTasksNode = 1 ; //number of processes per node (=1 default configuration)
  if (nDevNode > 0)
    {
    nTasksNode = params.find<int>("tasks_per_node",1); //number of processes per node
#ifdef HYPERQ
    // all processes in the same node share one GPU
    mydevID = 0;
    nTasksDev = nTasksNode;
    if (master) cout << "HyperQ enabled" << endl;
#else
    // only the first nDevNode processes of the node will use a GPU, each exclusively.
    mydevID = myID%nTasksNode; //ID within the node
    nTasksDev = 1;
    if (master)
       cout << "Configuration supported is 1 gpu for each mpi process" << endl;
    if (mydevID>=nDevNode)
      {
      cout << "There isn't a gpu available for process = " << myID << " computation will be performed on the host" << endl;
      mydevID = -1;
      }
#endif
    }
   else planck_fail("No GPUs are available");
#endif
#ifdef OPENCL
  // all processes use a number of GPUs >= 1 and <= nDevNode
  int nDevProc = params.find<int>("gpu_number",1);
  if (nDevProc > nDevNode)
  {
    if (master)
    {
      cout << "Number of GPUs available = " << nDevNode << " is lower than the number of GPUs required " << nDevProc << endl;
      cout << "Only " << nDevNode << " GPUs will be used per process" << endl;
    }
  }
#endif
  bool gpu_info = params.find<bool>("gpu_info",true);
  if (gpu_info)
    if (mydevID>=0) print_device_info(myID, mydevID);
#endif // CUDA || OPENCL

#ifdef SPLVISIVO
  get_colourmaps(params,amap,opt);
#else
  get_colourmaps(params,amap);
#endif

#ifdef MIC
  // Struct to hold reformatted particle data
  // Only used for Intel Xeon Phi offload model
  mic_soa_particles soa_particles;
#endif

  tstack_pop("Setup");
  string outfile, outfile2;
  bool a_eq_e = params.find<bool>("a_eq_e",true);
  int xres = params.find<int>("xres",800),
      yres = params.find<int>("yres",xres);
  arr2<COLOUR> pic(xres,yres), pic2(xres,yres);
  first=true;
  MPI_Status status;

  sceneMaker sMaker(params);
  while (sMaker.getNextScene (particle_data, r_points, campos, centerpos, lookat, sky, outfile))
    {

    bool background = params.find<bool>("background",false);
    pic.fill(COLOUR(0,0,0));
    if(background)
      {
	if (master)
	  {
	    cout << endl << "reading file Background/" << outfile << " ..." << endl;
	    
	    LS_Image img;
	    img.read_TGA("Background/"+outfile+".tga");

#pragma omp parallel for
	    for (tsize i=0; i<pic.size1(); ++i)
	      for (tsize j=0; j<pic.size2(); ++j)
		{
                  Colour8 c=img.get_pixel(i,j);
		  pic[i][j].r=c.r/256.0;
		  pic[i][j].g=c.g/256.0;
		  pic[i][j].b=c.b/256.0;
		}
	  }
	mpiMgr.bcastRaw(&pic[0][0].r,3*xres*yres);
      }

    tsize npart = particle_data.size();

    // Calculate boost factor for brightness
    bool boost = params.find<bool>("boost",false);
    float b_brightness = boost ?
      float(npart)/float(r_points.size()) : 1.0;

    if(npart>0)
    {
      // Use correct vector dependant on boost usage
      std::vector<particle_sim>* pData;
      if (boost) pData = &r_points;
      else       pData = &particle_data;

       tsize npart_all = npart;
       mpiMgr.allreduce (npart_all,MPI_Manager::Sum);

#if (defined(CUDA) || defined(OPENCL))
      // CUDA or OPENCL rendering
      if (mydevID >= 0)
      {
#ifdef CUDA
#ifdef ONLY_CUDA
        if (!a_eq_e) planck_fail("CUDA only supported for A==E so far");
        tstack_push("CUDA");
        cuda_rendering(mydevID, nTasksDev, pic, *pData, campos, centerpos, lookat, sky, amap, b_brightness, params);
        tstack_pop("CUDA");
#else
        tstack_push("CUDA");
        tstack_push("CUDA Split");
        split_r = params.find<float>("split_r", 0.0);
        size_t split = split_particle(&((*pData)[0]), &((*pData)[npart]));
        printf("myID=%d Split particles at %d/%d\n", myID, split, npart);
        tstack_pop("CUDA Split");

        tstack_push("CUDA Init Vector");
        std::vector<particle_sim> pData_cpu(split);
        std::vector<particle_sim> pData_gpu(npart-split);
        #pragma omp parallel for
        for (int i=0;i<split;i++)pData_cpu[i]=(*pData)[i];
        #pragma omp parallel for
        for (int i=split;i<npart;i++)pData_gpu[i-split]=(*pData)[i];
        //std::vector<particle_sim> pData_cpu((*pData).begin(), (*pData).begin()+split);
        //std::vector<particle_sim> pData_gpu((*pData).begin()+split+1, (*pData).end());
        //std::vector<particle_sim> &pData_ref = *pData;
        arr2<COLOUR> pic_cpu(xres,yres), pic_gpu(xres,yres);
        tstack_pop("CUDA Init Vector");

        std::thread gpu_render([mydevID, nTasksDev, &pic_gpu, &pData_gpu, campos, centerpos, lookat, sky, &amap, b_brightness, params]{
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                cuda_rendering(mydevID, nTasksDev, pic_gpu, pData_gpu, campos, centerpos, lookat, sky, amap, b_brightness, params);
                });
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(42, &cpuset);
        pthread_setaffinity_np(gpu_render.native_handle(), sizeof(cpu_set_t), &cpuset);
        int render_threads = params.find<int>("render_threads", 8);
        int threads = omp_get_max_threads();
        omp_set_num_threads(render_threads);
        host_rendering(params, pData_cpu, pic_cpu, campos, centerpos, lookat, sky, amap, b_brightness, npart_all);
        omp_set_num_threads(threads);
        tstack_push("CUDA Wait");
        gpu_render.join();
        tstack_pop("CUDA Wait");
        tstack_push("CUDA Merge");
        #pragma omp parallel for
        for (int ix=0;ix<xres;ix++)
            for (int iy=0;iy<yres;iy++)
            {
                pic[ix][iy].r=pic_cpu[ix][iy].r+pic_gpu[ix][iy].r;
                pic[ix][iy].g=pic_cpu[ix][iy].g+pic_gpu[ix][iy].g;
                pic[ix][iy].b=pic_cpu[ix][iy].b+pic_gpu[ix][iy].b;
            }
        tstack_pop("CUDA Merge");
        tstack_pop("CUDA");
#endif
#endif
#ifdef OPENCL
        tstack_push("OPENCL");
        opencl_rendering(mydevID, *pData, nDevProc, pic);
        tstack_pop("OPENCL");
#endif
      }
      else
      {
          host_rendering(params, *pData, pic, campos, centerpos, lookat, sky, amap, b_brightness, npart_all);
      }

#elif (defined(MIC))
      // Intel MIC rendering
      tstack_push("MIC");
      mic_rendering(params, *pData, pic, campos, centerpos, lookat, sky, amap, b_brightness,soa_particles, sMaker.is_final_scene());
      tstack_pop("MIC");
#else
      // Default host rendering
      host_rendering(params, *pData, pic, campos, centerpos, lookat, sky, amap, b_brightness, npart_all);
#endif
    }

    tstack_push("Post-processing");
    if(!first)MPI_Wait(&req, &status);
    pic.swap(pic2);
    mpiMgr.iallreduceRaw(reinterpret_cast<float *>(&pic2[0][0]),3*xres*yres,MPI_Manager::Sum, &req);

    if(first)
    {
      first=false;
      outfile2 = outfile;
      tstack_pop("Post-processing");
      continue;
    }
    else
    {
      exptable<float32> xexp(-20.0);
      if (mpiMgr.master() && a_eq_e)
  #pragma omp parallel for
        for (int ix=0;ix<xres;ix++)
          for (int iy=0;iy<yres;iy++)
            {
            pic[ix][iy].r=-xexp.expm1(pic[ix][iy].r);
            pic[ix][iy].g=-xexp.expm1(pic[ix][iy].g);
            pic[ix][iy].b=-xexp.expm1(pic[ix][iy].b);
            }
  
      tstack_replace("Post-processing","Output");
  
      if (master && params.find<bool>("colorbar",false))
        {
        cout << endl << "creating color bar ..." << endl;
        add_colorbar(params,pic,amap);
        }
  
      double gamma=params.find<double>("pic_gamma",1.0);
      double helligkeit=params.find<double>("pic_brighness",0.0);
      double kontrast=params.find<double>("pic_contrast",1.0);
  
      if (master && (gamma != 1.0 || helligkeit != 0.0 || kontrast != 1.0))
        {
  	cout << endl << "immage enhancement (gamma,brightness,contrast) = ("
  	     << gamma << "," << helligkeit << "," << kontrast << ")" << endl;
  #pragma omp parallel for
          for (tsize i=0; i<pic.size1(); ++i)
            for (tsize j=0; j<pic.size2(); ++j)
  	    {
  	      pic[i][j].r = kontrast * pow((double)pic[i][j].r,gamma) + helligkeit;
                pic[i][j].g = kontrast * pow((double)pic[i][j].g,gamma) + helligkeit;
                pic[i][j].b = kontrast * pow((double)pic[i][j].b,gamma) + helligkeit;
  	    }
         }
  
      if(!params.find<bool>("AnalyzeSimulationOnly"))
        {
        if (master)
          {
          cout << endl << "saving file " << outfile2 << " ..." << endl;
  
          LS_Image img(pic.size1(),pic.size2());
  
  #pragma omp parallel for
          for (tsize i=0; i<pic.size1(); ++i)
            for (tsize j=0; j<pic.size2(); ++j)
              img.put_pixel(i,j,Colour(pic[i][j].r,pic[i][j].g,pic[i][j].b));
          int pictype = params.find<int>("pictype",0);
          switch(pictype)
            {
            case 0:
              img.write_TGA(outfile2+".tga");
              break;
            case 1:
              planck_fail("ASCII PPM no longer supported");
              break;
            case 2:
              img.write_PPM(outfile2+".ppm");
              break;
            case 3:
              img.write_TGA_rle(outfile2+".tga");
              break;
            default:
              planck_fail("No valid image file type given ...");
              break;
            }
          }
        }
  
      outfile2 = outfile;
      tstack_pop("Output");
    }


  // Also meant to happen when using CUDA - unimplemented.
  #if (defined(OPENCL))
    cuda_timeReport();
  #endif
    timeReport();

    mpiMgr.barrier();
    // Abandon ship if a file named "stop" is found in the working directory.
    // ==>  Allows to stop rendering conveniently using a simple "touch stop".
    planck_assert (!file_present("stop"),"stop file found");
    }

  if(!first)
  {
    MPI_Wait(&req, &status);
    pic.swap(pic2);
    tstack_push("Post-processing");
  
    exptable<float32> xexp(-20.0);
    if (mpiMgr.master() && a_eq_e)
#pragma omp parallel for
        for (int ix=0;ix<xres;ix++)
          for (int iy=0;iy<yres;iy++)
            {
            pic[ix][iy].r=-xexp.expm1(pic[ix][iy].r);
            pic[ix][iy].g=-xexp.expm1(pic[ix][iy].g);
            pic[ix][iy].b=-xexp.expm1(pic[ix][iy].b);
            }
  
      tstack_replace("Post-processing","Output");
  
      if (master && params.find<bool>("colorbar",false))
        {
        cout << endl << "creating color bar ..." << endl;
        add_colorbar(params,pic,amap);
        }
  
      double gamma=params.find<double>("pic_gamma",1.0);
      double helligkeit=params.find<double>("pic_brighness",0.0);
      double kontrast=params.find<double>("pic_contrast",1.0);
  
      if (master && (gamma != 1.0 || helligkeit != 0.0 || kontrast != 1.0))
        {
            cout << endl << "immage enhancement (gamma,brightness,contrast) = ("
                << gamma << "," << helligkeit << "," << kontrast << ")" << endl;
#pragma omp parallel for
         for (tsize i=0; i<pic.size1(); ++i)
           for (tsize j=0; j<pic.size2(); ++j)
         {
           pic[i][j].r = kontrast * pow((double)pic[i][j].r,gamma) + helligkeit;
               pic[i][j].g = kontrast * pow((double)pic[i][j].g,gamma) + helligkeit;
               pic[i][j].b = kontrast * pow((double)pic[i][j].b,gamma) + helligkeit;
         }
        }
  
     if(!params.find<bool>("AnalyzeSimulationOnly"))
       {
       if (master)
         {
         cout << endl << "saving file " << outfile << " ..." << endl;
  
         LS_Image img(pic.size1(),pic.size2());
  
#pragma omp parallel for
        for (tsize i=0; i<pic.size1(); ++i)
          for (tsize j=0; j<pic.size2(); ++j)
            img.put_pixel(i,j,Colour(pic[i][j].r,pic[i][j].g,pic[i][j].b));
        int pictype = params.find<int>("pictype",0);
        switch(pictype)
          {
          case 0:
            img.write_TGA(outfile+".tga");
            break;
          case 1:
            planck_fail("ASCII PPM no longer supported");
            break;
          case 2:
            img.write_PPM(outfile+".ppm");
            break;
          case 3:
            img.write_TGA_rle(outfile+".tga");
            break;
          default:
            planck_fail("No valid image file type given ...");
            break;
          }
        }
      }
  
    tstack_pop("Output");
  #if (defined(OPENCL))
    cuda_timeReport();
  #endif
    timeReport();
  }
#ifdef VS
  //Just to hold the screen to read the messages when debugging
  cout << endl << "Press any key to end..." ;
  getchar();
#endif
  }
