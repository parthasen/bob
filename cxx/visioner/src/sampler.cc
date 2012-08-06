/**
 * @file visioner/src/sampler.cc
 * @date Fri 27 Jul 13:58:57 2012 CEST
 * @author Andre Anjos <andre.anjos@idiap.ch>
 *
 * @brief This file was part of Visioner and originally authored by "Cosmin
 * Atanasoaei <cosmin.atanasoaei@idiap.ch>". It was only modified to conform to
 * Bob coding standards and structure.
 *
 * Copyright (C) 2011-2012 Idiap Research Institute, Martigny, Switzerland
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/bind.hpp>
#include <boost/lambda/lambda.hpp>

#include "core/logging.h"

#include "visioner/model/sampler.h"
#include "visioner/model/mdecoder.h"

namespace bob { namespace visioner {

  // Constructor
  Sampler::Sampler(const param_t& param, SamplerType type)
    :	m_param(param),
    m_type(type),

    m_tagger(make_tagger(m_param)),
    m_loss(make_loss(m_param)),

    m_n_outputs(m_tagger->n_outputs()),
    m_n_samples(0),
    m_n_types(m_tagger->n_types()),

    m_rgens(n_threads(), boost::mt19937(param.m_seed))
    {
      stat_init(m_tcounts);
      stat_init(m_sprobs);

      const std::string& data = 
        type == TrainSampler ? param.m_train_data : param.m_valid_data;

      // Load the list files
      std::vector<std::string> ifiles, gfiles;
      if (	load_listfiles(data, ifiles, gfiles) == false ||
          ifiles.empty() || ifiles.size() != gfiles.size())
      {
        bob::core::warn << "Failed to load the datasets <" << data << ">!" << std::endl;
        ifiles.clear();
        gfiles.clear();
      }

      load(ifiles, gfiles);
    }

  // Reset to a set of listfiles
  void Sampler::load(const std::vector<std::string>& ifiles, const std::vector<std::string>& gfiles)
  {
    ipyramid_t ipyramid(m_param);

    std::vector<double> targets(n_outputs());
    uint64_t type;

    // Process each image in the list
    for (uint64_t i = 0; i < ifiles.size(); i ++)
    {
      bob::core::info << "mode [" << type2str()
        << "] loading image [" << (i + 1)  << "/" << ifiles.size() << "] ..."
        << std::endl;

      // Load the scaled images ...
      if (ipyramid.load(ifiles[i], gfiles[i]) == false)
      {
        bob::core::warn << "Failed to load the image <" << ifiles[i] << ">!"
          << std::endl;
        continue;
      }

      // Build the samples using sliding-windows
      for (uint64_t is = 0; is < ipyramid.size(); is ++)
      {
        const ipscale_t& ip = ipyramid[is];

        uint64_t new_n_samples = 0, old_n_samples = n_samples();
        for (int y = ip.m_scan_min_y; y < ip.m_scan_max_y; y += ip.m_scan_dy)
          for (int x = ip.m_scan_min_x; x < ip.m_scan_max_x; x += ip.m_scan_dx)
          {
            if (m_tagger->check(ip, x, y, targets, type) == true)
            {
              m_tcounts[type] ++;
              new_n_samples ++;
            }
          }

        // Make sure to store only images with at least one sample
        if (new_n_samples > 0)
        {
          m_ipscales.push_back(ip);
          m_ipsbegins.push_back(old_n_samples);
          m_ipsends.push_back(old_n_samples + new_n_samples);
          m_n_samples += new_n_samples;
        }

        //                                // Backgroung image - there is no point in keeping in memory too many scales!
        //                                if (ip.m_objects.empty() == true)
        //                                {
        //                                        is += 2;//ipyramid.size() / 8;
        //                                }
      }
    }

    // Debug
    for (uint64_t iti = 0; iti < n_types(); iti ++)
    {
      bob::core::info << "mode [" << type2str()
        << "] target type [" << iti << "]"
        << " found in " << m_tcounts[iti] << "/" << n_samples()
        << " samples." << std::endl;
    }
  }

  // Sample the given number of samples (uniformly)
  void Sampler::sample(uint64_t n_sel_samples, std::vector<uint64_t>& samples) const
  {
    // Compute the uniform sampling probabilities
    for (uint64_t iti = 0; iti < n_types(); iti ++)
    {
      m_sprobs[iti] = n_sel_samples * inverse(n_types()) * 
        inverse(m_tcounts[iti]);
    }

    // Split the computation (select the samples)
    std::vector<std::vector<uint64_t> > th_samples;                
    thread_iloop(
        boost::bind(&Sampler::th_usample, 
          this, boost::lambda::_1, boost::lambda::_2, boost::lambda::_3),
        n_samples(), th_samples);

    // Merge results
    samples.clear();
    for (uint64_t ith = 0; ith < n_threads(); ith ++)
    {
      samples.insert(samples.end(), th_samples[ith].begin(), th_samples[ith].end());
    }
    std::sort(samples.begin(), samples.end());
  }

  // Sample the given number of samples (error based)
  void Sampler::sample(uint64_t n_sel_samples, const Model& model, std::vector<uint64_t>& samples) const
  {
    // Split the computation (compute the error for each sample)
    std::vector<std::vector<double> > th_terrors;
    thread_loop(
        boost::bind(&Sampler::th_errors, 
          this, boost::lambda::_1, boost::cref(model), boost::lambda::_2),
        n_samples(), th_terrors);

    const std::vector<double> terrors = stat_cumulate(th_terrors);

    // Compute the error-based sampling probabilities
    for (uint64_t iti = 0; iti < n_types(); iti ++)
    {
      m_sprobs[iti] = n_sel_samples * inverse(m_tcounts.size()) *
        inverse(terrors[iti]);
    }                

    // Split the computation (select the samples)
    std::vector<std::vector<uint64_t> > th_samples;                
    thread_iloop(
        boost::bind(&Sampler::th_esample, 
          this, boost::lambda::_1, boost::lambda::_2, boost::cref(model), boost::lambda::_3),
        n_samples(), th_samples);

    // Merge results
    samples.clear();
    for (uint64_t ith = 0; ith < n_threads(); ith ++)
    {
      samples.insert(samples.end(), th_samples[ith].begin(), th_samples[ith].end());
    }
    std::sort(samples.begin(), samples.end());
  } 

  // Map selected samples to a dataset
  void Sampler::map(const std::vector<uint64_t>& _samples, const Model& model, DataSet& data) const
  {
    std::vector<uint64_t> samples = _samples;
    unique(samples);

    // Allocate memory
    data.resize(n_outputs(), samples.size(), model.n_features(), model.n_fvalues());

    // Split the computation (buffer the feature values and the targets)
    std::vector<uint64_t> types(samples.size(), 0);
    thread_loop(
        boost::bind(
          &Sampler::th_map, this, boost::lambda::_1,
          boost::cref(samples), boost::cref(model), boost::ref(types), boost::ref(data)),
        samples.size());

    // Compute the cost for each class
    std::vector<uint64_t> tcounts;
    stat_init(tcounts);

    for (uint64_t s = 0; s < samples.size(); s ++)
    {
      const uint64_t type = types[s];
      tcounts[type] ++;
    }

    double sum_inv = 0.0;
    for (uint64_t iti = 0; iti < n_types(); iti ++)
    {
      const uint64_t count = tcounts[iti];
      sum_inv += inverse(count);
    }                

    std::vector<double> tcosts;
    stat_init(tcosts);

    for (uint64_t iti = 0; iti < n_types(); iti ++)
    {
      const uint64_t type = iti;
      const uint64_t count = tcounts[iti];
      const double cost = inverse(sum_inv) * inverse(count) * tcounts.size();
      tcosts[iti] = cost;

      bob::core::info << "mode [" << type2str() << "] mapping target type ["
        << type << "] in " << count << "/" << samples.size() << " samples "
        << "with cost [" << cost << "]." << std::endl;
    }

    // Set the costs
    for (uint64_t s = 0; s < samples.size(); s ++)
    {
      const uint64_t type = types[s];
      data.cost(s) = tcosts[type];
    }
  }

  // Map the given sample to image
  uint64_t Sampler::sample2image(uint64_t s) const
  {
    for (uint64_t i = 0; i < n_images(); i ++)
    {
      if (    m_ipsbegins[i] <= s && 
          s < m_ipsends[i])
      {
        return i;
      }
    }

    return n_images();
  }

  // Compute the error of the given sample
  double Sampler::error(uint64_t x, uint64_t y, const std::vector<double>& targets, const Model& model, std::vector<double>& scores) const
  {
    for (uint64_t o = 0; o < n_outputs(); o ++)
    {
      scores[o] = model.score(o, x, y);
    }
    return m_loss->error(&targets[0], &scores[0], n_outputs()); 
  }

  // Cost-based sampling
  void Sampler::sample(uint64_t s, double cost, boost::mt19937& gen, boost::uniform_01<>& die, std::vector<uint64_t>& samples) const
  {
    uint64_t icost = (uint64_t)cost;
    if (icost > 0)
    {
      samples.insert(samples.end(), icost, s);
    }

    cost -= icost;
    if (die(gen) < cost)
    {
      samples.push_back(s);
    }
  }

  // Uniform sampling thread
  void Sampler::th_usample(uint64_t ith, std::pair<uint64_t, uint64_t> srange, std::vector<uint64_t>& samples) const
  {
    if (srange.first >= srange.second)
    {
      return;
    }

    std::vector<double> targets(n_outputs());
    uint64_t type;

    boost::mt19937& gen = m_rgens[ith];
    boost::uniform_01<> die;                

    // Process the valid samples in the range ...
    for (uint64_t i = sample2image(srange.first), s = m_ipsbegins[i]; 
        s < srange.second && i < n_images(); i ++)
    {
      const ipscale_t& ip = m_ipscales[i];

      for (int y = ip.m_scan_min_y; y < ip.m_scan_max_y; y += ip.m_scan_dy)
        for (int x = ip.m_scan_min_x; x < ip.m_scan_max_x; x += ip.m_scan_dx)
        {
          if (m_tagger->check(ip, x, y, targets, type) == true)
          {
            if (s >= srange.first && s < srange.second)
            {
              const double cost = m_sprobs[type];
              sample(s, cost, gen, die, samples);
            }
            s ++;                                        
          }
        }
    }
  }

  // Error-based sampling thread
  void Sampler::th_esample(uint64_t ith, std::pair<uint64_t, uint64_t> srange, const Model& bmodel, std::vector<uint64_t>& samples) const
  {
    if (srange.first >= srange.second)
    {
      return;
    }

    const boost::shared_ptr<Model> model = bmodel.clone();                
    std::vector<double> targets(n_outputs()), scores(n_outputs());
    uint64_t type;

    boost::mt19937& gen = m_rgens[ith];
    boost::uniform_01<> die;                

    // Process the valid samples in the range ...
    for (uint64_t i = sample2image(srange.first), s = m_ipsbegins[i]; 
        s < srange.second && i < n_images(); i ++) {

      const ipscale_t& ip = m_ipscales[i];

      model->preprocess(ip);

      for (int y = ip.m_scan_min_y; y < ip.m_scan_max_y; y += ip.m_scan_dy)
        for (int x = ip.m_scan_min_x; x < ip.m_scan_max_x; x += ip.m_scan_dx)
        {
          if (m_tagger->check(ip, x, y, targets, type) == true)
          {
            if (s >= srange.first && s < srange.second)
            {
              const double cost = 
                error(x, y, targets, *model, scores) * m_sprobs[type];
              sample(s, cost, gen, die, samples);
            }
            s ++;                                        
          }
        }
    }
  }

  // Evaluation thread
  void Sampler::th_errors(
      std::pair<uint64_t, uint64_t> srange, const Model& bmodel,
      std::vector<double>& terrors) const
  {
    if (srange.first >= srange.second)
    {
      return;
    }

    stat_init(terrors);

    const boost::shared_ptr<Model> model = bmodel.clone();                
    std::vector<double> targets(n_outputs()), scores(n_outputs());
    uint64_t type;

    // Process the valid samples in the range ...
    for (uint64_t i = sample2image(srange.first), s = m_ipsbegins[i]; 
        s < srange.second && i < n_images(); i ++)
    {
      const ipscale_t& ip = m_ipscales[i];

      model->preprocess(ip);

      for (int y = ip.m_scan_min_y; y < ip.m_scan_max_y; y += ip.m_scan_dy)
        for (int x = ip.m_scan_min_x; x < ip.m_scan_max_x; x += ip.m_scan_dx)
        {
          if (m_tagger->check(ip, x, y, targets, type) == true)
          {
            if (s >= srange.first && s < srange.second)
            {
              terrors[type] += error(x, y, targets, *model, scores);
            }
            s ++;                                        
          }
        }
    }
  }

  // Mapping thread (samples to dataset)
  void Sampler::th_map(
      std::pair<uint64_t, uint64_t> srange, 
      const std::vector<uint64_t>& samples, const Model& bmodel, 
      std::vector<uint64_t>& types, DataSet& data) const
  {
    if (srange.first >= srange.second)
    {
      return;
    }

    const boost::shared_ptr<Model> model = bmodel.clone();                
    std::vector<double> targets(n_outputs());
    uint64_t type;

    // Process the valid samples in the range ...                
    for (uint64_t ss = srange.first, i = sample2image(samples[ss]), s = m_ipsbegins[i]; 
        ss < srange.second && i < n_images(); i ++)
    {
      const ipscale_t& ip = m_ipscales[i];

      model->preprocess(ip);

      for (int y = ip.m_scan_min_y; y < ip.m_scan_max_y; y += ip.m_scan_dy)
        for (int x = ip.m_scan_min_x; x < ip.m_scan_max_x; x += ip.m_scan_dx)
        {
          if (m_tagger->check(ip, x, y, targets, type) == true)
          {
            if (ss < srange.second && s == samples[ss])
            {
              types[ss] = type;

              // Buffer targets
              for (uint64_t o = 0; o < n_outputs(); o ++)
              {
                data.target(ss, o) = targets[o];
              }

              // Buffer feature values
              for (uint64_t f = 0; f < model->n_features(); f ++)
              {
                data.value(f, ss) = model->get(f, x, y);
              }

              ss ++;

              // Debug - save the sample as an image
              //                        if (targets[0] < 0.0)
              //                                continue;

              //                        Matrix<uint8_t> image(m_param.m_rows, m_param.m_cols);
              //                        for (uint64_t y = 0; y < m_param.m_rows; y ++)
              //                        {
              //                                for (uint64_t x = 0; x < m_param.m_cols; x ++)
              //                                {
              //                                        image(y, x) = ipscale.m_image(sw.m_y + y, sw.m_x + x);
              //                                }
              //                        }

              //                        QImage qimage = convert(image);
              //                        qimage.save(QObject::tr("sample%1.target%2.png").arg(ss).arg(targets[0]));
            }

            s ++;                                        
          }
        }
    }
  }

}}
