#ifndef DEFACTO_LEARNER_BCD_LEARNER_H_
#define DEFACTO_LEARNER_BCD_LEARNER_H_
#include "difacto/learner.h"
#include "difacto/node_id.h"
#include "dmlc/data.h"
#include "data/chunk_iter.h"
#include "data/data_store.h"
#include "./bcd_param.h"
#include "./bcd_job.h"
#include "./bcd_utils.h"
#include "./tile_store.h"
#include "./tile_builder.h"
#include "loss/logit_loss_delta.h"
namespace difacto {

class BCDLearner : public Learner {
 public:
  BCDLearner() {
  }
  virtual ~BCDLearner() {
  }

  KWArgs Init(const KWArgs& kwargs) override {
  }
 protected:

  void RunScheduler() override {
    // load and convert data
    bool has_val = param_.data_val.size() != 0;
    CHECK(param_.data_in.size());
    IssueJobToWorkers(bcd::JobArgs::kPrepareTrainData, param_.data_in);

    if (has_val) {
      IssueJobToWorkers(bcd::JobArgs::kPrepareValData, param_.data_val);
    }

    epoch_ = 0;
    for (; epoch_ < param_.max_num_epochs; ++epoch_) {
      IssueJobToWorkers(bcd::JobArgs::kTraining);
      if (has_val) IssueJobToWorkers(bcd::JobArgs::kValidation);
    }
  }

  void Process(const std::string& args, std::string* rets) {
    bcd::JobArgs job(args);

    if (job.type == bcd::JobArgs::kPrepareValData ||
        job.type == bcd::JobArgs::kPrepareTrainData) {
      bcd::PrepDataRets prets;
      PrepareData(job, &prets);
      prets.SerializeToString(rets);
    } else if (job.type == bcd::JobArgs::kTraining ||
               job.type == bcd::JobArgs::kValidation) {
      // IterateFeatureBlocks(job, rets);
    }

    // if (job.type == kSaveModel) {
    // } else if (job.type == kLoadModel) {
    // } else {
    //   ProcessFile(job);
    // }
  }

 private:
  /**
   * \brief sleep for a moment
   * \param ms the milliseconds (1e-3 sec) for sleeping
   */
  inline void Sleep(int ms = 1000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }

  /**
   * \brief send jobs to workers and wait them finished.
   */
  void IssueJobToWorkers(int job_type,
                         const std::string& filename = "") {
    // job_type_ = job_type;
    // for (auto& cb : before_epoch_callbacks_) cb();
    // int nworker = model_store_->NumWorkers();
    // std::vector<Job> jobs(nworker);
    // for (int i = 0; i < nworker; ++i) {
    //   jobs[i].type = job_type;
    //   jobs[i].filename = filename;
    //   jobs[i].num_parts = nworker;
    //   jobs[i].part_idx = i;
    //   jobs[i].node_id = NodeID::Encode(NodeID::kWorkerGroup, i);
    // }
    // job_tracker_->Add(jobs);
    // while (job_tracker_->NumRemains() != 0) { Sleep(); }
    // for (auto& cb : epoch_callbacks_) cb();
  }

  void PrepareData(const bcd::JobArgs& job, bcd::PrepDataRets* rets) {
    // read and process a 512MB chunk each time
    ChunkIter reader(job.filename, param_.data_format,
                     job.part_idx, job.num_parts, 1<<28);

    bcd::FeaGroupStats stats(param_.num_feature_group_bits);
    bcd::TileBuilder* builder;

    while (reader.Next()) {
      auto rowblk = reader.Value();
      stats.Add(rowblk);
      builder->Add(rowblk);
      pred_.push_back(SArray<real_t>(rowblk.size));
    }

    // push the feature ids and feature counts to the servers
    int t = model_store_->Push(
        Store::kFeaCount, builder->feaids, builder->feacnts, SArray<int>());
    model_store_->Wait(t);
    builder->feacnts.clear();

    // report statistics to the scheduler
    stats.Get(&(CHECK_NOTNULL(rets)->feablk_avg));
  }

  void BuildFeatureMap(const bcd::JobArgs& job) {
    bcd::TileBuilder* builder;

    // pull the aggregated feature counts from the servers
    SArray<real_t> feacnt;
    int t = model_store_->Pull(
        Store::kFeaCount, builder->feaids, &feacnt, nullptr);
    model_store_->Wait(t);

    // remove the filtered features
    SArray<feaid_t> filtered;
    size_t n = builder->feaids.size();
    CHECK_EQ(feacnt.size(), n);
    for (size_t i = 0; i < n; ++i) {
      if (feacnt[i] > param_.tail_feature_filter) {
        filtered.push_back(builder->feaids[i]);
      }
    }

    // build colmap for each rowblk
    builder->feaids = filtered;
    builder->BuildColmap(job.fea_blk_ranges);

    // init aux data
    std::vector<Range> pos;
    bcd::FindPosition(filtered, job.fea_blk_ranges, &pos);
    feaids_.resize(pos.size());
    delta_.resize(pos.size());
    model_offset_.resize(pos.size());

    for (int i = 0; i < pos.size(); ++i) {
      feaids_[i] = filtered.segment(pos[i].begin, pos[i].end);
      delta_[i].resize(feaids_[i].size());
    }
  }

  void IterateFeatureBlocks(const bcd::JobArgs& job, bcd::IterFeaBlkRets* rets) {
    CHECK(job.fea_blks.size());
    // hint for data prefetch
    for (int f : job.fea_blks) {
      for (int d = 0; d < num_data_blks_; ++d) {
        tile_store_->Prefetch(d, f);
      }
    }

    size_t nfeablk = job.fea_blks.size();
    int tau = 0;
    FeatureBlockTracker feablk_tracker(nfeablk);
    for (size_t i = 0; i < nfeablk; ++i) {
      auto on_complete = [&feablk_tracker, i]() {
        feablk_tracker.Finish(i);
      };
      int f = job.fea_blks[i];
      IterateFeablk(f, on_complete);

      if (i >= tau) feablk_tracker.Wait(i - tau);
    }
    for (int i = nfeablk - tau; i < nfeablk ; ++i) feablk_tracker.Wait(i);
  }

  /**
   * \brief iterate a feature block
   *
   * the logic is as following
   *
   * 1. calculate gradident
   * 2. push gradients to servers, so servers will update the weight
   * 3. once the push is done, pull the changes for the weights back from
   *    the servers
   * 4. once the pull is done update the prediction
   *
   * however, two things make the implementation is not so intuitive.
   *
   * 1. we need to iterate the data block one by one for both calcluating
   * gradient and update prediction
   * 2. we used callbacks to avoid to be blocked by the push and pull.
   *
   * NOTE: once cannot iterate on the same block before it is actually finished.
   *
   * @param blk_id
   * @param on_complete will be called when actually finished
   */
  void IterateFeablk(int blk_id, const std::function<void()>& on_complete) {
    // 1. calculate gradient
    SArray<int> grad_offset = model_offset_[blk_id];
    // we compute both 1st and diagnal 2nd gradients. it's ok to overwrite model_offset_
    for (int& os : grad_offset) os += os;
    SArray<real_t> grad(grad_offset.back());
    for (int i = 0; i < num_data_blks_; ++i) {
      CalcGrad(i, blk_id, grad_offset, &grad);
    }

    // 3. once push is done, pull the changes for the weights
    // this callback will be called when the push is finished
    auto push_callback = [this, blk_id, on_complete]() {
      // must use pointer here, since it may be reallocated by model_store_
      SArray<real_t>* delta_w = new SArray<real_t>();
      SArray<int>* delta_w_offset = new SArray<int>();
      // 4. once the pull is done, update the prediction
      // the callback will be called when the pull is finished
      auto pull_callback = [this, blk_id, delta_w, delta_w_offset, on_complete]() {
        model_offset_[blk_id] = *delta_w_offset;
        for (int i = 0; i < num_data_blks_; ++i) {
          UpdtPred(i, blk_id, *delta_w_offset, *delta_w);
        }
        delete delta_w;
        delete delta_w_offset;
        on_complete();
      };
      // pull the changes of w from the servers
      model_store_->Pull(
          Store::kWeight, feaids_[blk_id], delta_w, delta_w_offset, pull_callback);
    };
    // 2. push gradient to the servers
    model_store_->Push(
        Store::kGradient, feaids_[blk_id], grad, grad_offset, push_callback);
  }

  void CalcGrad(int rowblk_id, int colblk_id,
                const SArray<int>& grad_offset,
                SArray<real_t>* grad) {
    bcd::Tile tile;
    tile_store_->Fetch(rowblk_id, colblk_id, &tile);

    size_t n = tile.colmap.size();
    SArray<int> grad_pos(n);
    SArray<real_t> delta(n);
    for (size_t i = 0; i < n; ++i) {
      int map = tile.colmap[i];
      grad_pos[i] = grad_offset[map];
      delta[i] = delta_[colblk_id][map];
    }

    loss_->CalcGrad(tile.data, {SArray<char>(pred_[rowblk_id]),
            SArray<char>(grad_pos), SArray<char>(delta)}, grad);
  }

  void UpdtPred(int rowblk_id, int colblk_id, const SArray<int> delta_w_offset,
                const SArray<real_t> delta_w) {
    bcd::Tile tile;
    tile_store_->Fetch(rowblk_id, colblk_id, &tile);

    size_t n = tile.colmap.size();
    SArray<int> w_pos(n);
    for (size_t i = 0; i < n; ++i) {
      w_pos[i] = delta_w_offset[tile.colmap[i]];
    }

    loss_->Predict(tile.data,
                   {SArray<char>(delta_w_offset), SArray<char>(w_pos)},
                   &pred_[rowblk_id]);
  }

  /** \brief the current epoch */
  int epoch_;
  /** \brief the current job type */
  int job_type_;
  int num_data_blks_ = 0;
  /** \brief the model store*/
  Store* model_store_ = nullptr;
  Loss* loss_;
  /** \brief data store */
  DataStore* data_store_ = nullptr;
  bcd::TileStore* tile_store_ = nullptr;

  /** \brief parameters */
  BCDLearnerParam param_;

  std::vector<SArray<real_t>> pred_;
  std::vector<SArray<feaid_t>> feaids_;
  std::vector<SArray<real_t>> delta_;
  std::vector<SArray<int>> model_offset_;

  /**
   * \brief monitor if or not a feature block is finished
   */
  class FeatureBlockTracker {
   public:
    FeatureBlockTracker(int num_blks) : done_(num_blks) { }
    /** \brief mark id as finished */
    void Finish(int id) {
      mu_.lock();
      done_[id] = 1;
      mu_.unlock();
      cond_.notify_all();
    }
    /** \brief block untill id is finished */
    void Wait(int id) {
      std::unique_lock<std::mutex> lk(mu_);
      cond_.wait(lk, [this, id] {return done_[id] == 1; });
    }
   private:
    std::mutex mu_;
    std::condition_variable cond_;
    std::vector<int> done_;
  };

};

}  // namespace difacto
#endif  // DEFACTO_LEARNER_BCD_LEARNER_H_
