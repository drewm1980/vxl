// This is brl/bseg/boxm2/ocl/algo/boxm2_ocl_update_belief.cxx
#include "boxm2_ocl_update_belief.h"
//:
// \file
// \brief  A process for updating a color model
//
// \author Vishal Jain
// \date Mar 25, 2011

#include <vcl_fstream.h>
#include <vcl_algorithm.h>
#include <boxm2/ocl/boxm2_opencl_cache.h>
#include <boxm2/boxm2_scene.h>
#include <boxm2/boxm2_block.h>
#include <boxm2/boxm2_data_base.h>
#include <boxm2/ocl/boxm2_ocl_util.h>
#include <boxm2/boxm2_util.h>
#include <boxm2/ocl/algo/boxm2_ocl_camera_converter.h>
#include <vil/vil_image_view.h>
#include <vil/vil_save.h>
#include <vcl_sstream.h>
#include <vcl_iomanip.h>

//directory utility
#include <vul/vul_timer.h>
#include <vcl_where_root_dir.h>
#include <bocl/bocl_device.h>
#include <bocl/bocl_kernel.h>
#include <vnl/vnl_numeric_traits.h>
#include <vnl/vnl_random.h>

//: Map of kernels should persist between process executions
vcl_map<vcl_string,vcl_vector<bocl_kernel*> > boxm2_ocl_update_belief::kernels_;

//Main public method, updates color model
bool boxm2_ocl_update_belief::update(boxm2_scene_sptr         scene,
                              bocl_device_sptr         device,
                              boxm2_opencl_cache_sptr  opencl_cache,
                              vpgl_camera_double_sptr  cam,
                              vil_image_view_base_sptr img,
                              unsigned                 image_index,
                              vil_image_view_base_sptr mask_sptr,
                              float                    mog_var,
                              bool					   update_app,
                              bool					   update_occ
                              )
{
  enum {
	UPDATE_SEGLEN = 0,
	COMPUTE_PI_INTEGRALS = 1,
	UPDATE_PREINF = 2,
	UPDATE_PROC   = 3,
	UPDATE_BAYES  = 4,
	UPDATE_CELL   = 5
  };

  float transfer_time=0.0f;
  float gpu_time=0.0f;
  vcl_size_t local_threads[2]={8,8};
  vcl_size_t global_threads[2]={8,8};


  float resnearfactor = 1e6f;
  float resfarfactor = 1e-6f;
  vcl_size_t               startI=0;
  vcl_size_t               startJ=0;


  //catch a "null" mask (not really null because that throws an error)
  bool use_mask = false;
  if ( mask_sptr->ni() == img->ni() && mask_sptr->nj() == img->nj() ) {
    vcl_cout<<"Update using mask."<<vcl_endl;
    use_mask = true;
  }
  vil_image_view<unsigned char >* mask_map = 0;
  if (use_mask) {
    mask_map = dynamic_cast<vil_image_view<unsigned char> *>(mask_sptr.ptr());
    if (!mask_map) {
      vcl_cout<<"boxm2_update_process:: mask map is not an unsigned char map"<<vcl_endl;
      return false;
    }
  }

  //cache size sanity check
  vcl_size_t binCache = opencl_cache.ptr()->bytes_in_cache();
  vcl_cout<<"Update MBs in cache: "<<binCache/(1024.0*1024.0)<<vcl_endl;

  //make correct data types are here
  vcl_string data_type, num_obs_type,options;
  int appTypeSize;
  bool isRGB = false;
  if (!validate_appearances(scene, data_type, appTypeSize, num_obs_type, options, isRGB))
    return false;

  // create a command queue.
  int status=0;
  cl_command_queue queue = clCreateCommandQueue( device->context(),
                                                 *(device->device_id()),
                                                 CL_QUEUE_PROFILING_ENABLE,
                                                 &status);
  if (status!=0)
    return false;

  // compile the kernel if not already compiled
  vcl_vector<bocl_kernel*>& kernels = get_kernels(device, options);

  //grab input image, establish cl_ni, cl_nj (so global size is divisible by local size)
  vil_image_view_base_sptr float_img = boxm2_util::prepare_input_image(img, true);
  vil_image_view<float>* img_view = static_cast<vil_image_view<float>* >(float_img.ptr());
  unsigned cl_ni=(unsigned)RoundUp(img_view->ni(),(int)local_threads[0]);
  unsigned cl_nj=(unsigned)RoundUp(img_view->nj(),(int)local_threads[1]);
  global_threads[0]=cl_ni;
  global_threads[1]=cl_nj;

  //set generic cam
  cl_float* ray_origins    = new cl_float[4*cl_ni*cl_nj];
  cl_float* ray_directions = new cl_float[4*cl_ni*cl_nj];
  bocl_mem_sptr ray_o_buff = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(cl_float4), ray_origins, "ray_origins buffer");
  bocl_mem_sptr ray_d_buff = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(cl_float4), ray_directions, "ray_directions buffer");
  boxm2_ocl_camera_converter::compute_ray_image( device, queue, cam, cl_ni, cl_nj, ray_o_buff, ray_d_buff, startI, startJ);

  float tnearfar[2] = { 0.0f, 1000000} ;

  if(cam->type_name() == "vpgl_perspective_camera")
  {
      float f  = ((vpgl_perspective_camera<double> *)cam.ptr())->get_calibration().focal_length()*((vpgl_perspective_camera<double> *)cam.ptr())->get_calibration().x_scale();
      tnearfar[0] = f* scene->finest_resolution()/resnearfactor ;
      tnearfar[1] = f* scene->finest_resolution()/resfarfactor ;

      // vcl_cout<<"Near and Far Clipping planes "<<tnearfar[0]<<" "<<tnearfar[1]<<vcl_endl;
  }
  bocl_mem_sptr tnearfar_mem_ptr = opencl_cache->alloc_mem(2*sizeof(float), tnearfar, "tnearfar  buffer");
  tnearfar_mem_ptr->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);
  //Visibility, Preinf, Norm, and input image buffers
  float* vis_buff = new float[cl_ni*cl_nj];
  float* pre_buff = new float[cl_ni*cl_nj];
  float* norm_buff = new float[cl_ni*cl_nj];
  float* input_buff=new float[cl_ni*cl_nj];
  for (unsigned i=0;i<cl_ni*cl_nj;i++)
  {
    vis_buff[i]=1.0f;
    pre_buff[i]=0.0f;
    norm_buff[i]=0.0f;
  }

  //copy input vals into image
  int count=0;
  for (unsigned int j=0;j<cl_nj;++j) {
    for (unsigned int i=0;i<cl_ni;++i) {
      input_buff[count] = 0.0f;
      if ( i<img_view->ni() && j< img_view->nj() )
        input_buff[count] = (*img_view)(i,j);
      ++count;
    }
  }


  //bocl_mem_sptr in_image=new bocl_mem(device->context(),input_buff,cl_ni*cl_nj*sizeof(float),"input image buffer");
  bocl_mem_sptr in_image = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float), input_buff, "input image buffer");
  in_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  //bocl_mem_sptr vis_image=new bocl_mem(device->context(),vis_buff,cl_ni*cl_nj*sizeof(float),"vis image buffer");
  bocl_mem_sptr vis_image = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float), vis_buff, "vis image buffer");
  vis_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  //bocl_mem_sptr pre_image=new bocl_mem(device->context(),pre_buff,cl_ni*cl_nj*sizeof(float),"pre image buffer");
  bocl_mem_sptr pre_image = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float), pre_buff, "pre image buffer");
  pre_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  //bocl_mem_sptr norm_image=new bocl_mem(device->context(),norm_buff,cl_ni*cl_nj*sizeof(float),"pre image buffer");
  bocl_mem_sptr norm_image = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float), norm_buff, "norm image buffer");
  norm_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  // Image Dimensions
  int img_dim_buff[4];
  img_dim_buff[0] = 0;
  img_dim_buff[1] = 0;
  img_dim_buff[2] = img_view->ni();
  img_dim_buff[3] = img_view->nj();

  bocl_mem_sptr img_dim=new bocl_mem(device->context(), img_dim_buff, sizeof(int)*4, "image dims");
  img_dim->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  // Output Array
  float output_arr[100];
  for (int i=0; i<100; ++i) output_arr[i] = 0.0f;
  bocl_mem_sptr  cl_output=new bocl_mem(device->context(), output_arr, sizeof(float)*100, "output buffer");
  cl_output->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

  // bit lookup buffer
  cl_uchar lookup_arr[256];
  boxm2_ocl_util::set_bit_lookup(lookup_arr);
  bocl_mem_sptr lookup=new bocl_mem(device->context(), lookup_arr, sizeof(cl_uchar)*256, "bit lookup buffer");
  lookup->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

  vcl_cout << "Update app: " << update_app << "." << vcl_endl;
  bocl_mem_sptr update_app_mem = new bocl_mem(device->context(), &update_app, sizeof(bool), "update_app_mem");
  update_app_mem->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

  vcl_cout << "Update occ: " << update_occ << "." << vcl_endl;
  bocl_mem_sptr update_occ_mem = new bocl_mem(device->context(), &update_occ, sizeof(bool), "update_occ_mem");
  update_occ_mem->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

  vnl_random rand(9667566); //init with good seed
  unsigned num_samples = 128;
  unsigned num_random_numbers = num_samples;
  float normal_random_numbers[num_random_numbers];
  for (int i=0; i<num_random_numbers; ++i){
   normal_random_numbers[i] = rand.normal();
  }
  bocl_mem_sptr  cl_normal_random_numbers=new bocl_mem(device->context(), normal_random_numbers, sizeof(float)*num_random_numbers, "normal_random_numbers buffer");
  cl_normal_random_numbers->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

  bocl_mem_sptr num_samples_mem = new bocl_mem(device->context(), &num_samples, sizeof(num_samples), "inum_samples");
  num_samples_mem->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

  // set arguments
  vcl_vector<boxm2_block_id> vis_order;
  if(cam->type_name() == "vpgl_perspective_camera")
      vis_order= scene->get_vis_blocks_opt((vpgl_perspective_camera<double>*)cam.ptr(),img_view->ni(),img_view->nj());
  else
      vis_order= scene->get_vis_blocks(cam);
  vcl_vector<boxm2_block_id>::iterator id;
  float prev_gpu_time = 0;
  for (unsigned int i=0; i< kernels.size(); ++i)
  {
	prev_gpu_time = gpu_time;
    if ( i == UPDATE_PROC )
    {
      bocl_kernel * proc_kern=kernels[i];
      proc_kern->set_arg( norm_image.ptr() );
      proc_kern->set_arg( in_image.ptr() );
      proc_kern->set_arg( vis_image.ptr() );
      proc_kern->set_arg( pre_image.ptr());
      proc_kern->set_arg( img_dim.ptr() );

      //execute kernel
      proc_kern->execute( queue, 2, local_threads, global_threads);
      int status = clFinish(queue);
      if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
        return false;
      proc_kern->clear_args();
//
      norm_image->read_to_buffer(queue);

      int count = 0;
    }

    //set masked values
    vis_image->read_to_buffer(queue);
    if (use_mask)
    {
      int count = 0;
      for (unsigned int j=0;j<cl_nj;++j) {
        for (unsigned int i=0;i<cl_ni;++i) {
          if ( i<mask_map->ni() && j<mask_map->nj() ) {
            if ( (*mask_map)(i,j)==0 ) {
              input_buff[count] = -1.0f;
              vis_buff  [count] = 0.0f;
            }
          }
          ++count;
        }
      }
      in_image->write_to_buffer(queue);
      vis_image->write_to_buffer(queue);
      clFinish(queue);
    }

    for (id = vis_order.begin(); id != vis_order.end(); ++id)
    {
      //choose correct render kernel
      boxm2_block_metadata mdata = scene->get_block_metadata(*id);
      bocl_kernel* kern = kernels[i];

      //write the image values to the buffer
      vul_timer transfer;
      bocl_mem* blk       = opencl_cache->get_block(scene,*id);
      bocl_mem* blk_info  = opencl_cache->loaded_block_info();
      bocl_mem* alpha     = opencl_cache->get_data<BOXM2_ALPHA>(scene,*id,0,false);
      boxm2_scene_info* info_buffer = (boxm2_scene_info*) blk_info->cpu_buffer();
      int alphaTypeSize = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_ALPHA>::prefix());
      info_buffer->data_buffer_length = (int) (alpha->num_bytes()/alphaTypeSize);
      blk_info->write_to_buffer((queue));

      int nobsTypeSize = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_NUM_OBS>::prefix());
      // data type string may contain an identifier so determine the buffer size
      bocl_mem* mog       = opencl_cache->get_data(scene,*id,data_type,alpha->num_bytes()/alphaTypeSize*appTypeSize,false);    //info_buffer->data_buffer_length*boxm2_data_info::datasize(data_type));
      bocl_mem* num_obs   = opencl_cache->get_data(scene,*id,num_obs_type,alpha->num_bytes()/alphaTypeSize*nobsTypeSize,false);//,info_buffer->data_buffer_length*boxm2_data_info::datasize(num_obs_type));

      //grab an appropriately sized AUX data buffer
      int auxTypeSize = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX0>::prefix());
      bocl_mem *aux0   = opencl_cache->get_data<BOXM2_AUX0>(scene,*id, info_buffer->data_buffer_length*auxTypeSize);

      auxTypeSize = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX1>::prefix());
      bocl_mem *aux1   = opencl_cache->get_data<BOXM2_AUX1>(scene,*id, info_buffer->data_buffer_length*auxTypeSize);

      //create suffix
      vcl_stringstream ss;
      ss << "img_" <<  vcl_setfill('0') << vcl_setw(5) << image_index;
      vcl_string suffix = ss.str();

      int msgTypeSize = boxm2_data_info::datasize(boxm2_data_traits<BOXM2_LOG_MSG>::prefix());
      bocl_mem* messages   = opencl_cache->get_data(scene,*id, boxm2_data_traits<BOXM2_LOG_MSG>::prefix(suffix),info_buffer->data_buffer_length*msgTypeSize,false);

      auxTypeSize = boxm2_data_info::datasize(boxm2_data_traits<BOXM2_SUM_LOG_MSG>::prefix());
      bocl_mem * pos_log_msg_sum   = opencl_cache->get_data(scene,*id,  boxm2_data_traits<BOXM2_SUM_LOG_MSG>::prefix("pos"), info_buffer->data_buffer_length*auxTypeSize, false);

	  auxTypeSize = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX3>::prefix());
	  bocl_mem *PI_array   = opencl_cache->get_data<BOXM2_AUX3>(scene,*id, info_buffer->data_buffer_length*auxTypeSize);


      transfer_time += (float) transfer.all();
      if (i==UPDATE_SEGLEN)
      {
        aux0->zero_gpu_buffer(queue);
        aux1->zero_gpu_buffer(queue);

        kern->set_arg( blk_info );
        kern->set_arg( blk );
        kern->set_arg( alpha );
        kern->set_arg( aux0 );
        kern->set_arg( aux1 );
        kern->set_arg( lookup.ptr() );
        kern->set_arg( ray_o_buff.ptr() );
        kern->set_arg( ray_d_buff.ptr() );
        kern->set_arg( tnearfar_mem_ptr.ptr() );
        kern->set_arg( img_dim.ptr() );
        kern->set_arg( in_image.ptr() );
        kern->set_arg( cl_output.ptr() );
        kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar16) );//local tree,
        kern->set_local_arg( local_threads[0]*local_threads[1]*10*sizeof(cl_uchar) ); //cumsum buffer, imindex buffer

        //execute kernel
        kern->execute(queue, 2, local_threads, global_threads);
        int status = clFinish(queue);
        if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
          return false;
        gpu_time += kern->exec_time();

        //clear render kernel args so it can reset em on next execution
        kern->clear_args();
      }
      else if (i==COMPUTE_PI_INTEGRALS)
		{
		  local_threads[0] = 64;
		  local_threads[1] = 1 ;
		  global_threads[0]=RoundUp(info_buffer->data_buffer_length,local_threads[0]);
		  global_threads[1]=1;

		  kern->set_arg( blk_info );
		  kern->set_arg( mog );
		  kern->set_arg( aux0 );
		  kern->set_arg( aux1 );
		  kern->set_arg( PI_array );
		  kern->set_arg( cl_normal_random_numbers.ptr() );
		  kern->set_arg( num_samples_mem.ptr() );

		  //execute kernel
		  kern->execute(queue, 2, local_threads, global_threads);
		  int status = clFinish(queue);
		  if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
			return false;
		  gpu_time += kern->exec_time();

		  //clear render kernel args so it can reset em on next execution
		  kern->clear_args();
		}
      else if (i==UPDATE_PREINF)
      {
  		local_threads[0] = 8;
  		local_threads[1] = 8;
  		global_threads[0]=cl_ni;
  		global_threads[1]=cl_nj;

        kern->set_arg( blk_info );
        kern->set_arg( blk );
        kern->set_arg( alpha );
        kern->set_arg( messages );
        kern->set_arg( mog );
        kern->set_arg( num_obs );
        kern->set_arg( aux0 );
        kern->set_arg( PI_array );
        kern->set_arg( pos_log_msg_sum );
        kern->set_arg( lookup.ptr() );
        kern->set_arg( ray_o_buff.ptr() );
        kern->set_arg( ray_d_buff.ptr() );
        kern->set_arg( tnearfar_mem_ptr.ptr() );
        kern->set_arg( img_dim.ptr() );
        kern->set_arg( vis_image.ptr() );
        kern->set_arg( pre_image.ptr() );
        kern->set_arg( cl_output.ptr() );
        kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar16) );//local tree,
        kern->set_local_arg( local_threads[0]*local_threads[1]*10*sizeof(cl_uchar) ); //cumsum buffer, imindex buffer
        //execute kernel
        kern->execute(queue, 2, local_threads, global_threads);
        int status = clFinish(queue);
        if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
          return false;
        gpu_time += kern->exec_time();

        //clear render kernel args so it can reset em on next execution
        kern->clear_args();

      }
      else if (i==UPDATE_BAYES)
      {
        //vis
        auxTypeSize = boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX2>::prefix());
        bocl_mem *aux2   = opencl_cache->get_data<BOXM2_AUX2>(scene,*id, info_buffer->data_buffer_length*auxTypeSize);
        aux2->zero_gpu_buffer(queue);

        auxTypeSize = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX4>::prefix());
        bocl_mem *new_msg = opencl_cache->get_data<BOXM2_AUX4>(scene,*id, info_buffer->data_buffer_length*auxTypeSize);
        new_msg->zero_gpu_buffer(queue);


        kern->set_arg( blk_info );
        kern->set_arg( blk );
        kern->set_arg( alpha );
        kern->set_arg( messages );
        kern->set_arg( mog );
        kern->set_arg( num_obs );
        kern->set_arg( aux0 );
        kern->set_arg( PI_array );
        kern->set_arg( aux2 );
        kern->set_arg( pos_log_msg_sum );
        kern->set_arg( new_msg );
        kern->set_arg( lookup.ptr() );
        kern->set_arg( ray_o_buff.ptr() );
        kern->set_arg( ray_d_buff.ptr() );
        kern->set_arg( tnearfar_mem_ptr.ptr() );
        kern->set_arg( img_dim.ptr() );
        kern->set_arg( vis_image.ptr() );
        kern->set_arg( pre_image.ptr() );
        kern->set_arg( norm_image.ptr() );
        kern->set_arg( cl_output.ptr() );
        kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar16) );//local tree,
        kern->set_local_arg( local_threads[0]*local_threads[1]*10*sizeof(cl_uchar) ); //cumsum buffer, imindex buffer
        //execute kernel
        kern->execute(queue, 2, local_threads, global_threads);
        int status = clFinish(queue);
        if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
          return false;
        gpu_time += kern->exec_time();

        //clear render kernel args so it can reset em on next execution
        kern->clear_args();
      }
      else if (i==UPDATE_CELL)
      {
        auxTypeSize = boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX2>::prefix());
        bocl_mem *aux2   = opencl_cache->get_data<BOXM2_AUX2>(scene,*id, info_buffer->data_buffer_length*auxTypeSize);

        auxTypeSize = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_AUX4>::prefix());
        bocl_mem *new_msg = opencl_cache->get_data<BOXM2_AUX4>(scene,*id, info_buffer->data_buffer_length*auxTypeSize);

		bocl_mem *PI_array   = opencl_cache->get_data<BOXM2_AUX3>(scene,*id, info_buffer->data_buffer_length*auxTypeSize);


        //mog variance, if 0.0f or less, then var will be learned
        bocl_mem_sptr mog_var_mem = new bocl_mem(device->context(), &mog_var, sizeof(mog_var), "update gauss variance");
        mog_var_mem->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

        local_threads[0] = 64;
        local_threads[1] = 1 ;
        global_threads[0]=RoundUp(info_buffer->data_buffer_length,local_threads[0]);
        global_threads[1]=1;

        kern->set_arg( blk_info );
        kern->set_arg( alpha );
        kern->set_arg( mog );
        kern->set_arg( num_obs );
        kern->set_arg( aux0 );
        kern->set_arg( aux1 );
        kern->set_arg( aux2 );
        kern->set_arg( PI_array );

        kern->set_arg( pos_log_msg_sum );
        kern->set_arg( new_msg );
        kern->set_arg( messages );
        kern->set_arg( mog_var_mem.ptr() );
        kern->set_arg( update_app_mem.ptr() );
        kern->set_arg( update_occ_mem.ptr() );
        kern->set_arg( cl_output.ptr() );

        //execute kernel
        kern->execute(queue, 2, local_threads, global_threads);
        int status = clFinish(queue);
        if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
          return false;
        gpu_time += kern->exec_time();

        //clear render kernel args so it can reset em on next execution
        kern->clear_args();

        //write info to disk
        alpha->read_to_buffer(queue);
        mog->read_to_buffer(queue);
        num_obs->read_to_buffer(queue);
        messages->read_to_buffer(queue);
        pos_log_msg_sum->read_to_buffer(queue);

        opencl_cache->deep_remove_data(scene,*id, boxm2_data_traits<BOXM2_LOG_MSG>::prefix(suffix));
      }

      //read image out to buffer (from gpu)
      in_image->read_to_buffer(queue);
      vis_image->read_to_buffer(queue);
      pre_image->read_to_buffer(queue);
      clFinish(queue);
    }

    vcl_cout << "Kernel " << i << " took " << gpu_time - prev_gpu_time << vcl_endl;
  }

  delete [] vis_buff;
  delete [] pre_buff;
  delete [] norm_buff;
  delete [] input_buff;
  delete [] ray_origins;
  delete [] ray_directions;
  opencl_cache->unref_mem(in_image.ptr());
  opencl_cache->unref_mem(vis_image.ptr());
  opencl_cache->unref_mem(pre_image.ptr());
  opencl_cache->unref_mem(norm_image.ptr());
  opencl_cache->unref_mem(ray_o_buff.ptr());
  opencl_cache->unref_mem(ray_d_buff.ptr());
  opencl_cache->unref_mem(tnearfar_mem_ptr.ptr());
  vcl_cout<<"Gpu time "<<gpu_time<<" transfer time "<<transfer_time<<vcl_endl;
  clReleaseCommandQueue(queue);
  return true;
}


//Returns vector of color update kernels (and caches them per device
vcl_vector<bocl_kernel*>& boxm2_ocl_update_belief::get_kernels(bocl_device_sptr device, vcl_string opts)
{
  // compile kernels if not already compiled
  vcl_string identifier = device->device_identifier() + opts;
  if (kernels_.find(identifier) != kernels_.end())
    return kernels_[identifier];

  //otherwise compile the kernels
  vcl_cout<<"=== boxm2_ocl_update_belief_process::compiling kernels on device "<<identifier<<"==="<<vcl_endl;

  vcl_vector<vcl_string> src_paths;
  vcl_string source_dir = boxm2_ocl_util::ocl_src_root();
  src_paths.push_back(source_dir + "scene_info.cl");
  src_paths.push_back(source_dir + "pixel_conversion.cl");
  src_paths.push_back(source_dir + "bit/bit_tree_library_functions.cl");
  src_paths.push_back(source_dir + "backproject.cl");
  src_paths.push_back(source_dir + "atomics_util.cl");
  src_paths.push_back(source_dir + "statistics_library_functions.cl");
  src_paths.push_back(source_dir + "bit/bp_kernels.cl");
  vcl_vector<vcl_string> non_ray_src = vcl_vector<vcl_string>(src_paths);

  //push ray trace files
  src_paths.push_back(source_dir + "bp_functors.cl");
  src_paths.push_back(source_dir + "bit/cast_ray_bit.cl");

  //compilation options
  vcl_string options = /*"-D ATOMIC_FLOAT " +*/ opts;

  //populate vector of kernels
  vcl_vector<bocl_kernel*> vec_kernels;

  //seg len pass
  bocl_kernel* seg_len = new bocl_kernel();
  vcl_string seg_opts = options + " -D SEGLEN  -D STEP_CELL=step_cell_seglen(aux_args,data_ptr,llid,d)";
  seg_len->create_kernel(&device->context(), device->device_id(), src_paths, "seg_len_main", seg_opts, "update::seg_len");
  vec_kernels.push_back(seg_len);

  //may need DIFF LIST OF SOURCES FOR THSI GUY TOO
  bocl_kernel* compute_pi_integral = new bocl_kernel();
  vcl_string compute_pi_integral_opts = options + " -D COMPUTE_PI_INTEGRALS ";
  compute_pi_integral->create_kernel(&device->context(), device->device_id(), non_ray_src, "compute_pi_integral", compute_pi_integral_opts, "update::compute_pi_integral");
  vec_kernels.push_back(compute_pi_integral);

  bocl_kernel* pre_inf = new bocl_kernel();
  vcl_string pre_opts = options + " -D PREINF  -D STEP_CELL=step_cell_preinf(aux_args,data_ptr,llid,d)";
  pre_inf->create_kernel(&device->context(), device->device_id(), src_paths, "pre_inf_main", pre_opts, "update::pre_inf");
  vec_kernels.push_back(pre_inf);

  //may need DIFF LIST OF SOURCES FOR THIS GUY
  bocl_kernel* proc_img = new bocl_kernel();
  vcl_string proc_opts = options + " -D PROC_NORM ";
  proc_img->create_kernel(&device->context(), device->device_id(), non_ray_src, "proc_norm_image", proc_opts, "update::proc_norm_image");
  vec_kernels.push_back(proc_img);

  //push back cast_ray_bit
  bocl_kernel* bayes_main = new bocl_kernel();
  vcl_string bayes_opt = options + " -D BAYES  -D STEP_CELL=step_cell_bayes(aux_args,data_ptr,llid,d)";
  bayes_main->create_kernel(&device->context(), device->device_id(), src_paths, "bayes_main", bayes_opt, "update::bayes_main");
  vec_kernels.push_back(bayes_main);

  //may need DIFF LIST OF SOURCES FOR THSI GUY TOO
  bocl_kernel* update = new bocl_kernel();
  vcl_string update_opts = options + " -D UPDATE_BIT_SCENE_MAIN";
  update->create_kernel(&device->context(), device->device_id(), non_ray_src, "update_bit_scene_main", update_opts, "update::update_main");
  vec_kernels.push_back(update);

  //store and return
  kernels_[identifier] = vec_kernels;
  return kernels_[identifier];
}


//makes sure appearance types correspond correctly
bool boxm2_ocl_update_belief::validate_appearances(boxm2_scene_sptr scene,
                                            vcl_string& data_type,
                                            int& appTypeSize,
                                            vcl_string& num_obs_type,
                                            vcl_string& options,
                                            bool& isRGB)
{
  vcl_vector<vcl_string> apps = scene->appearances();
  bool foundDataType = false, foundNumObsType = false;
  for (unsigned int i=0; i<apps.size(); ++i) {
    if ( apps[i] == boxm2_data_traits<BOXM2_MOG3_GREY>::prefix() )
    {
      data_type = apps[i];
      foundDataType = true;
      options=" -D MOG_TYPE_8";
      appTypeSize = (int)boxm2_data_info::datasize(boxm2_data_traits<BOXM2_MOG3_GREY>::prefix());
    }
    else if ( apps[i] == boxm2_data_traits<BOXM2_NUM_OBS>::prefix() )
    {
      num_obs_type = apps[i];
      foundNumObsType = true;
    }
  }
  if (!foundDataType) {
    vcl_cout<<"BOXM2_OPENCL_UPDATE_PROCESS ERROR: scene doesn't have BOXM2_MOG3_GREY or BOXM2_MOG3_GREY_16 data type"<<vcl_endl;
    return false;
  }
  if (!foundNumObsType) {
    vcl_cout<<"BOXM2_OPENCL_UPDATE_PROCESS ERROR: scene doesn't have BOXM2_NUM_OBS type"<<vcl_endl;
    return false;
  }
  return true;
}
