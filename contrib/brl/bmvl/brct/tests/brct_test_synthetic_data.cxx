// This is brl/bmvl/brct/tests/brct_test_synthetic_data.cxx
#include <vcl_iostream.h>
#include <vcl_fstream.h>
#include <vcl_vector.h>
#include <vcl_string.h>
#include <vnl/vnl_math.h>
#include <vil1/vil1_save.h>
#include <vil1/vil1_memory_image_of.h>
#include <vdgl/vdgl_edgel_chain.h>
#include <vdgl/vdgl_interpolator.h>
#include <vdgl/vdgl_digital_curve.h>
#include <vdgl/vdgl_digital_curve_sptr.h>
#include <vsol/vsol_point_2d.h>
#include "../kalman_filter.h"

#define Assert(x) { vcl_cout << #x "\t\t\t test "; \
  if (x) { ++success; vcl_cout << "PASSED\n"; } else { ++failures; vcl_cout << "FAILED\n"; } }

bool near_eq(double x, double y){return vcl_fabs(x-y)<0.1;}
static double x1(double t)
{
  return 0.707*t;
}
static double z1(double t)
{
  return (10-0.707*t);
}
static double x2(double t)
{
  return (0.707*t + 7.07);
}
static double z2(double t)
{
  return (17.07-0.707*t);
}
static vsol_point_2d_sptr project_point(vnl_double_3x4 const& P, const double x, const double y, const double z)
{
  vnl_matrix<double> X(4,1);
  X[0][0]=x;   X[1][0]=y;   X[2][0]=z; X[3][0]=1; 
  vnl_matrix<double> p = P*X;
  if(vcl_fabs(p[2][0])<1e-06)
    return 0;
  double u = p[0][0]/p[2][0], v = p[1][0]/p[2][0];
  return new vsol_point_2d(u, v);
}

static void 
generate_tracks(vnl_double_3x4 const& P,
                vcl_vector<vcl_vector<vdgl_digital_curve_sptr> >& tracks)
{
  double delta = 1.0;
  double ymin = 2, ymax = 4;
  vcl_vector<vdgl_digital_curve_sptr> track1, track2;
  double t1min=-7, t1max = 5;
  vcl_cout << "Track1 \n" ;
  for(double t1 = t1min; t1<=t1max; t1+=delta)
    {
      double xs=x1(t1), ys=ymin, zs=z1(t1);
      double xe=xs, ye=ymax, ze=zs;
      vcl_cout << "Xs(" << xs << " " << ys << " " << zs << ")\n";
      vsol_point_2d_sptr ps = project_point(P, xs, ys, zs);
      vsol_point_2d_sptr pe = project_point(P, xe, ye, ze);
      vdgl_digital_curve_sptr c = new vdgl_digital_curve(ps, pe);
      track1.push_back(c);
    }
  tracks.push_back(track1);
  vcl_cout << "\n\n Track2 \n" ;
  double t2min=-9, t2max = 2;
  for(double t2 = t2min; t2<=t2max; t2+=delta)
    {
      double xs=x2(t2), ys=ymin, zs=z2(t2);
      double xe=xs, ye=ymax, ze=zs;
      vcl_cout << "Xs(" << xs << " " << ys << " " << zs << ")\n";
      vsol_point_2d_sptr ps = project_point(P, xs, ys, zs);
      vsol_point_2d_sptr pe = project_point(P, xe, ye, ze);
      vdgl_digital_curve_sptr c = new vdgl_digital_curve(ps, pe);
      track2.push_back(c);
    }
  tracks.push_back(track2);
}
static vnl_double_3x3 generate_K()
{
  vnl_double_3x3 K;
  // set up the intrinsic matrix of the camera
  K[0][0] = 841.3804; K[0][1] = 0;        K[0][2] = 331.0916;
  K[1][0] = 0;        K[1][1] = 832.7951; K[1][2] = 221.5451;
  K[2][0] = 0;        K[2][1] = 0;        K[2][2] = 1;
  return K;
}
static vnl_double_3x4 generate_P(vnl_double_3x3 const & K)
{
  vnl_double_3x4 P;
  P[0][0] = 1;       P[0][1] = 0;        P[0][2] = 0;          P[0][3] = 0;
  P[1][0] = 0;       P[1][1] = 1;        P[1][2] = 0;          P[1][3] = 0;
  P[2][0] = 0;       P[2][1] = 0;        P[2][2] = 1;          P[2][3] = 0;
  return K*P;
}
static void 
write_track_to_image(vcl_vector<vdgl_digital_curve_sptr> const& track,
                     vil1_memory_image_of<unsigned char>& image)
{
  int w = image.width(), h = image.height();
  for(vcl_vector<vdgl_digital_curve_sptr>::const_iterator cit = track.begin();
      cit != track.end(); cit++)
    {
      vdgl_digital_curve_sptr dc = (*cit);
      vdgl_interpolator_sptr intp =dc->get_interpolator();
      vdgl_edgel_chain_sptr chain = intp->get_edgel_chain();
      int n = chain->size();
      for(int i = 0; i<n; i++)
        {
          int x = (int)(*chain)[i].x(), y =(int)(*chain)[i].y();
          if(x<0||x>=w)
            continue;
          if(y<0||y>=h)
            continue;
          image(x,y) = 100;
        }
    }
}
static void 
write_track_to_file(vcl_vector<vdgl_digital_curve_sptr> const& track,
                    vcl_string const & file)
{
  vcl_ofstream str(file.c_str());
  if(!str)
    return;
  str << "CURVE\n";
  for(vcl_vector<vdgl_digital_curve_sptr>::const_iterator cit = track.begin();
      cit != track.end(); cit++)
    {
      vdgl_digital_curve_sptr dc = (*cit);
      vdgl_interpolator_sptr intp =dc->get_interpolator();
      vdgl_edgel_chain_sptr chain = intp->get_edgel_chain();
      int n = chain->size();
      str << "[BEGIN CONTOUR]\n";
      str << "EDGE_COUNT=" << n << "\n";
      for(int i = 0; i<n; i++)
        {
          int x = (*chain)[i].x(), y = (*chain)[i].y();
          str << "[" << x << ", " << y << "] " << 0.0 << " " << 100 << "\n";
        }
      str << "[END CONTOUR]\n";
    }
  str << "[END CURVE]\n";
  str.close();
}
int main(int argc, char * argv[])
{
//  vcl_string file = "c:/videos/PoliceCar2/track_data/par.txt";
  vcl_string path = "c:/vxl/vxl/contrib/brl/bmvl/brct/tests/";
  int success=0, failures=0;
  kalman_filter kf;
  vnl_double_3x4 P = generate_P(generate_K());
  vcl_vector<vcl_vector<vdgl_digital_curve_sptr> > tracks;
  generate_tracks(P, tracks);
  vil1_memory_image_of<unsigned char> track_image(1024, 768);
  for(vcl_vector<vcl_vector<vdgl_digital_curve_sptr> >::iterator trit =
        tracks.begin(); trit != tracks.end(); trit++)
    {
      kf.add_track(*trit);
      write_track_to_image(*trit, track_image);
    }
  vcl_string image_file = path + "tracks.tif";
  vil1_save(track_image, image_file.c_str(), "tiff");
  vcl_string track_file = path + "track0.txt";
  write_track_to_file(tracks[0], track_file);
  track_file = path + "track1.txt";
  write_track_to_file(tracks[1], track_file);

  vcl_cout << "Test Summary: " << success << " tests succeeded, "
           << failures << " tests failed" << (failures?"\t***\n":"\n");
  return failures;
}

