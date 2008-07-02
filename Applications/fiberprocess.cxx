/*=========================================================================

  Program:   NeuroLib (DTI command line tools)
  Language:  C++
  Date:      $Date: 2008-07-02 15:54:54 $
  Version:   $Revision: 1.4 $
  Author:    Casey Goodlett (gcasey@sci.utah.edu)

  Copyright (c)  Casey Goodlett. All rights reserved.
  See NeuroLibCopyright.txt or http://www.ia.unc.edu/dev/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
// STL includes
#include <string>
#include <iostream>
#include <fstream>

// boost includes
#include <boost/program_options/option.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/positional_options.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/cmdline.hpp>

// ITK includes
#include <itkDiffusionTensor3D.h>
#include <itkImageFileReader.h>
#include <itkImageFileWriter.h>
#include <itkTensorLinearInterpolateImageFunction.h>
#include <itkVectorLinearInterpolateImageFunction.h>
#include <itkVersion.h>

//#include "FiberCalculator.h"
#include "deformationfieldoperations.h"
#include "fiberio.h"
#include "dtitypes.h"

namespace po = boost::program_options;

int main(int argc, char* argv[])
{
  // Read program options/configuration
  po::options_description config("Usage: fiberprocess input-fiber [options]");
  config.add_options()
    ("help,h", "produce this help message")
    ("verbose,v", "produces verbose output")

    ("fiber-output,o", po::value<std::string>(), "Output fiber file.  May be warped or updated with new data depending on other options used.")
    ("h-field,H", po::value<std::string>(), "HField for warp and statistics lookup.  If this option is used tensor-volume must also be specified.")
    ("no-warp,n", "Do not warp the geometry of the tensors only obtain the new statistics")
    ("tensor-volume,T", po::value<std::string>(), "Interpolate tensor values from the given field")

    // ******  TODO **********
    ("voxelize,V", po::value<std::string>(),"Voxelize fiber into a label map.  The tensor file must be specified to get the size, origin, spacing of the image.")
    ("voxelize-count-fibers", "Count number of fibers per-voxel instead of just setting to 1")
    ("voxel-label,l", po::value<ScalarPixelType>()->default_value(1),"Label for voxelized fiber")

    // Compute 1-D statistics
    // ("mean-statistics,s", po::value<std::string>(), "Write summary statistics to text file")
    // ******  TODO **********
    ;

  po::options_description hidden("Hidden options");
  hidden.add_options()
    ("fiber-file", po::value<std::string>(), "DTI fiber file")
    ;

  po::options_description all;
  all.add(config).add(hidden);

  po::positional_options_description p;
  p.add("fiber-file",1);

  po::variables_map vm;

  try
  {
    po::store(po::command_line_parser(argc, argv).
              options(all).positional(p).run(), vm);
    po::notify(vm);     
  } 
  catch (const po::error &e)
  {
    std::cout << "Parse error: " << std::endl;
    std::cout << config << std::endl;
    return EXIT_FAILURE;
  }

  // End option reading configuration

  // Display help if asked or program improperly called
  if(vm.count("help") || !vm.count("fiber-file"))
  {
    std::cout << config << std::endl;
    if(vm.count("help"))
    {
      std::cout << "Version $Revision: 1.4 $ "<< std::endl;
      std::cout << ITK_SOURCE_VERSION << std::endl;
      return EXIT_SUCCESS;
    }
    else
    {
      return EXIT_FAILURE;
    }
  }

  const bool VERBOSE = vm.count("verbose");

  // Reader fiber bundle
  GroupType::Pointer group = readFiberFile(vm["fiber-file"].as<std::string>());

  DeformationImageType::Pointer deformationfield(NULL);
  if(vm.count("h-field"))
    deformationfield = readDeformationField(vm["h-field"].as<std::string>(), HField);
//  else if(vm.count("displacement-field"))
//    deformationfield = readDeformationField(vm["displacement-field"].as<std::string>(), Displacement);
  else
    deformationfield = NULL;

  typedef itk::VectorLinearInterpolateImageFunction<DeformationImageType, double> DeformationInterpolateType;
  DeformationInterpolateType::Pointer definterp(NULL);
  if(deformationfield)
  {
    definterp = DeformationInterpolateType::New();
    definterp->SetInputImage(deformationfield);
  }

  // Setup new fiber bundle group
  GroupType::Pointer newgroup = GroupType::New();
  newgroup->SetId(0);

  ChildrenListType* children = group->GetChildren(0);

  if(VERBOSE)
    std::cout << "Getting spacing" << std::endl;

  // Get Spacing and offset from group
  const double* spacing = group->GetSpacing();
  newgroup->SetSpacing(spacing);

  const itk::Vector<double, 3> sooffset = group->GetObjectToParentTransform()->GetOffset();
  newgroup->GetObjectToParentTransform()->SetOffset(sooffset.GetDataPointer());

  // Setup tensor file if available
  typedef itk::ImageFileReader<TensorImageType> TensorImageReader;
  typedef itk::TensorLinearInterpolateImageFunction<TensorImageType, double> TensorInterpolateType;
  TensorImageReader::Pointer tensorreader = NULL;
  TensorInterpolateType::Pointer tensorinterp = NULL;
  
  if(vm.count("tensor-volume"))
  {
    tensorreader = TensorImageReader::New();
    tensorinterp = TensorInterpolateType::New();

    tensorreader->SetFileName(vm["tensor-volume"].as<std::string>().c_str());
    try
    {
      tensorreader->Update();
      tensorinterp->SetInputImage(tensorreader->GetOutput());
    }
    catch(itk::ExceptionObject exp)
    {
      std::cerr << exp << std::endl;
      return EXIT_FAILURE;
    }
  }

  if(VERBOSE)
    std::cout << "Starting Loop" << std::endl;

  ChildrenListType::iterator it;
  unsigned int id = 1;

  // Need to allocate an image to write into for creating
  // the fiber label map
  IntImageType::Pointer labelimage;
  if(vm.count("voxelize"))
  {
    if(!vm.count("tensor-volume"))
    {
      std::cerr << "Must specify tensor file to copy image metadata for fiber voxelize." << std::endl;
      return EXIT_FAILURE;
    }
    tensorreader->GetOutput();
    labelimage = IntImageType::New();
    labelimage->SetSpacing(tensorreader->GetOutput()->GetSpacing());
    labelimage->SetOrigin(tensorreader->GetOutput()->GetOrigin());
    labelimage->SetDirection(tensorreader->GetOutput()->GetDirection());
    labelimage->SetRegions(tensorreader->GetOutput()->GetLargestPossibleRegion());
    labelimage->Allocate();
    labelimage->FillBuffer(0);
  }

  // For each fiber
  for(it = children->begin(); it != children->end(); it++)
  {
    DTIPointListType pointlist = dynamic_cast<DTITubeType*>((*it).GetPointer())->GetPoints();
    DTITubeType::Pointer newtube = DTITubeType::New();
    DTIPointListType newpoints;
    
    DTIPointListType::iterator pit;

    // For each point alogng thje fiber
    for(pit = pointlist.begin(); pit != pointlist.end(); ++pit)
    {
      DTIPointType newpoint;
      typedef DTIPointType::PointType PointType;

      // p is not really a point its a continuous index
      const PointType p = pit->GetPosition();
      typedef DeformationInterpolateType::ContinuousIndexType ContinuousIndexType;
      ContinuousIndexType ci, origci;
      for(unsigned int i =0; i < 3; i++)
        origci[i] = ci[i] = p[i];

      if(deformationfield)
      {
        
        DeformationPixelType warp(definterp->EvaluateAtContinuousIndex(ci).GetDataPointer());
        
        for(unsigned int i =0; i < 3; i++)
          ci[i] = ci[i] + warp[i] / spacing[i];
      }

      if(vm.count("voxelize"))
      {
        itk::Point<double, 3> pt;
        pt[0] = ci[0] * spacing[0] + sooffset[0];
        pt[1] = ci[1] * spacing[1] + sooffset[1];
        pt[2] = ci[2] * spacing[2] + sooffset[2];

        ContinuousIndexType cind;
        itk::Index<3> ind;
        labelimage->TransformPhysicalPointToContinuousIndex(pt, cind);
        ind[0] = static_cast<long int>(round(cind[0]));
        ind[1] = static_cast<long int>(round(cind[1]));
        ind[2] = static_cast<long int>(round(cind[2]));
        if(!labelimage->GetLargestPossibleRegion().IsInside(ind))
        {
          std::cerr << "Error index: " << ind << " not in image"  << std::endl;
          return EXIT_FAILURE;
        }
        if(vm.count("voxelize-count-fibers"))
          labelimage->SetPixel(ind, labelimage->GetPixel(ind) + 1);
        else
          labelimage->SetPixel(ind, vm["voxel-label"].as<ScalarPixelType>());
      }
      
      // Should not have to do this
      if(vm.count("no-warp"))
        newpoint.SetPosition(origci);
      else
        newpoint.SetPosition(ci);

      newpoint.SetRadius(.4);
      newpoint.SetRed(0.0);
      newpoint.SetGreen(1.0);
      newpoint.SetBlue(0.0);

      // Attribute tensor data if provided
      if(vm.count("tensor-volume") && vm.count("fiber-output"))
      {
        itk::DiffusionTensor3D<double> tensor(tensorinterp->EvaluateAtContinuousIndex(ci).GetDataPointer());

        // TODO: Change SpatialObject interface to accept DiffusionTensor3D
        float sotensor[6];
        for(unsigned int i = 0; i < 6; ++i)
          sotensor[i] = tensor[i];

        newpoint.SetTensorMatrix(sotensor);

        typedef itk::DiffusionTensor3D<double>::EigenValuesArrayType EigenValuesType;
        EigenValuesType eigenvalues;
        tensor.ComputeEigenValues(eigenvalues);
        
        newpoint.AddField(itk::DTITubeSpatialObjectPoint<3>::FA, tensor.GetFractionalAnisotropy());
        newpoint.AddField("md", tensor.GetTrace()/3);
        newpoint.AddField("fro", sqrt(tensor[0]*tensor[0] +
                                      2*tensor[1]*tensor[1] +
                                      2*tensor[2]*tensor[2] +
                                      tensor[3]*tensor[3] +
                                      2*tensor[4]*tensor[4] +
                                      tensor[5]*tensor[5]));
        newpoint.AddField("l1", eigenvalues[0]);
        newpoint.AddField("l2", eigenvalues[1]);
        newpoint.AddField("l3", eigenvalues[2]);
      }
      else
      {
        newpoint.SetTensorMatrix(pit->GetTensorMatrix());
      }

      newpoints.push_back(newpoint);
    }

    newtube->SetSpacing(spacing);
    newtube->SetId(id++);
    newtube->SetPoints(newpoints);
    newgroup->AddSpatialObject(newtube);
  }

  newgroup->ComputeObjectToWorldTransform();

  if(VERBOSE)
    std::cout << "Ending Loop" << std::endl;

  if(VERBOSE)
    std::cout << "Output: " << vm["fiber-output"].as<std::string>() << std::endl;

  if(vm.count("fiber-output"))
    writeFiberFile(vm["fiber-output"].as<std::string>(), newgroup);

  if(vm.count("voxelize"))
  {
    typedef itk::ImageFileWriter<IntImageType> LabelWriter;
    LabelWriter::Pointer writer = LabelWriter::New();
    writer->SetInput(labelimage);
    writer->SetFileName(vm["voxelize"].as<std::string>());
    writer->UseCompressionOn();
    try
    {
      writer->Update();
    }
    catch(itk::ExceptionObject & e)
    {
      std::cerr << e.what() << std::endl;
      return EXIT_FAILURE;
    }
  }

  delete children;
  return EXIT_SUCCESS;
}