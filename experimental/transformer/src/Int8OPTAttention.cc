#include "Int8OPTAttention.h"

#include "operators.h"
#include "utils.h"

Int8OPTAttention::Int8OPTAttention(int embed_dim, int num_heads, BMM_S8T_S8N_F32T &qk_bmm, BMM_S8T_S8N_S8T &pv_bmm,
                                   W8A8B8O8Linear &k_proj, W8A8B8O8Linear &v_proj, W8A8B8O8Linear &q_proj,
                                   W8A8BFP32OFP32Linear &out_proj) {
    this->embed_dim = embed_dim;
    this->num_heads = num_heads;
    this->head_dim = embed_dim / num_heads;
    this->qk_bmm = qk_bmm;
    this->pv_bmm = pv_bmm;
    this->k_proj = k_proj;
    this->v_proj = v_proj;
    this->q_proj = q_proj;
    this->out_proj = out_proj;
}

Int8OPTAttention::Int8OPTAttention(std::string param_path, int embed_dim, int num_heads, BMM_S8T_S8N_F32T &qk_bmm,
                                   BMM_S8T_S8N_S8T &pv_bmm, W8A8B8O8Linear &k_proj, W8A8B8O8Linear &v_proj,
                                   W8A8B8O8Linear &q_proj, W8A8BFP32OFP32Linear &out_proj) {
    load_BMM_S8T_S8N_F32T(qk_bmm, param_path + "/qk_bmm");
    load_BMM_S8T_S8N_S8T(pv_bmm, param_path + "/pv_bmm");
    load_W8A8B8O8Linear_params(k_proj, param_path + "/k_proj");
    load_W8A8B8O8Linear_params(v_proj, param_path + "/v_proj");
    load_W8A8B8O8Linear_params(q_proj, param_path + "/q_proj");
    load_W8A8BFP32OFP32Linear_params(out_proj, param_path + "/out_proj");

    this->embed_dim = embed_dim;
    this->num_heads = num_heads;
    this->head_dim = embed_dim / num_heads;
    this->qk_bmm = qk_bmm;
    this->pv_bmm = pv_bmm;
    this->k_proj = k_proj;
    this->v_proj = v_proj;
    this->q_proj = q_proj;
    this->out_proj = out_proj;
}

void Int8OPTAttention::shpae(Matrix3D<int8_t> unshape, Matrix3D<int8_t> shaped, int sqlen) {
    assert(unshape.m_dim_x == 1);  // bsz == 1
    assert(unshape.m_dim_y == sqlen);
    assert(unshape.m_dim_z == this->num_heads * this->head_dim);
    assert(shaped.m_dim_x == this->num_heads);
    assert(shaped.m_dim_y == sqlen);
    assert(shaped.m_dim_z == this->head_dim);

    for (int i = 0; i < this->num_heads; i++) {
        for (int j = 0; j < sqlen; j++) {
            for (int k = 0; k < this->head_dim; k++) {
                shaped(i, j, k) = unshape(0, j, i * this->head_dim + k);
            }
        }
    }
}

void Int8OPTAttention::unshape(Matrix3D<int8_t> shaped, Matrix3D<int8_t> unshape, int sqlen) {
    assert(unshape.m_dim_x == 1);  // bsz == 1
    assert(unshape.m_dim_y == sqlen);
    assert(unshape.m_dim_z == this->num_heads * this->head_dim);
    assert(shaped.m_dim_x == this->num_heads);
    assert(shaped.m_dim_y == sqlen);
    assert(shaped.m_dim_z == this->head_dim);

    for (int i = 0; i < this->num_heads; i++) {
        for (int j = 0; j < sqlen; j++) {
            for (int k = 0; k < this->head_dim; k++) {
                unshape(0, j, i * this->head_dim + k) = shaped(i, j, k);
            }
        }
    }
}

template <typename T>
void transpose_1_2idx(Matrix3D<T> input, Matrix3D<T> output) {
    assert(input.m_dim_x == output.m_dim_x);
    assert(input.m_dim_y == output.m_dim_z);
    assert(input.m_dim_z == output.m_dim_y);

    for (int i = 0; i < input.m_dim_x; i++) {
        for (int j = 0; j < input.m_dim_y; j++) {
            for (int k = 0; k < input.m_dim_z; k++) {
                output(i, k, j) = input(i, j, k);
            }
        }
    }
}

#define HEAD 12
#define MAXSQLEN 512
float attn_weights_arr[HEAD * MAXSQLEN * MAXSQLEN];
float attn_weightsGT_arr[HEAD * MAXSQLEN * MAXSQLEN];
float attn_probs_arr[HEAD * MAXSQLEN * MAXSQLEN];
int8_t attn_probs_int8_arr[HEAD * MAXSQLEN * MAXSQLEN];
// TODO: var allocation method
struct Int8OPTAttention_output Int8OPTAttention::forward(const struct Int8OPTAttention_input &input) {
    struct Int8OPTAttention_output output;
    const int sqlen = input.hidden_states.m_dim_y, b = input.hidden_states.m_dim_x;
    assert(b == 1);

    int8_t query_states_unshape_arr[sqlen * this->embed_dim];
    Matrix3D<int8_t> query_states_unshape(query_states_unshape_arr, b, sqlen, embed_dim);
    // opt.py: query_states = self.q_proj(hidden_states)
    this->q_proj.forward(input.hidden_states, query_states_unshape);

    // opt.py: key_states = self._shape(self.k_proj(hidden_states), -1, bsz)
    int8_t key_states_unshape_arr[sqlen * this->embed_dim];
    Matrix3D<int8_t> key_states_unshape(key_states_unshape_arr, b, sqlen, embed_dim);
    this->k_proj.forward(input.hidden_states, key_states_unshape);
    int8_t key_states_arr[b * sqlen * this->embed_dim];
    Matrix3D<int8_t> key_states(key_states_arr, this->num_heads, sqlen, this->head_dim);
    this->shpae(key_states_unshape, key_states, sqlen);

    // opt.py: value_states = self._shape(self.v_proj(hidden_states), -1, bsz)
    int8_t value_states_unshape_arr[sqlen * this->embed_dim];
    Matrix3D<int8_t> value_states_unshape(value_states_unshape_arr, b, sqlen, embed_dim);
    this->v_proj.forward(input.hidden_states, value_states_unshape);
    int8_t value_states_arr[sqlen * this->embed_dim];
    Matrix3D<int8_t> value_states(value_states_arr, this->num_heads, sqlen, this->head_dim);
    this->shpae(value_states_unshape, value_states, sqlen);

    if (input.past_key != NULL && input.past_value != NULL) {
        // # reuse k, v, self_attention
        // key_states = torch.cat([past_key_value[0], key_states], dim=2)
        // value_states = torch.cat([past_key_value[1], value_states], dim=2)
    }

    // opt.py: query_states = self._shape(query_states, tgt_len, bsz).view(*proj_shape)
    int8_t query_states_arr[sqlen * this->embed_dim];
    Matrix3D<int8_t> query_states(query_states_arr, this->num_heads, sqlen, this->head_dim);
    this->shpae(query_states_unshape, query_states, sqlen);

    // opt.py: attn_weights = self.qk_bmm(query_states, key_states)
    // float attn_weights_arr[this->num_heads * sqlen * sqlen];
    // TODO: check src_len and tgt_len in generative mode
    Matrix3D<float> attn_weights(attn_weights_arr, this->num_heads, sqlen, sqlen);
    this->qk_bmm.forward(query_states, key_states, attn_weights);

    // opt.py: attn_weights = attn_weights.view(bsz, self.num_heads, tgt_len, src_len) + attention_mask
    batch_Add(attn_weights, input.attention_mask, attn_weights);

    // opt.py: attn_probs = nn.functional.softmax(attn_weights, dim=-1)
    // float attn_probs_arr[this->num_heads * sqlen * sqlen];
    Matrix3D<float> attn_probs(attn_weights_arr, this->num_heads, sqlen, sqlen);
    softmax(attn_weights, attn_probs, 2);

    // TODO: do we need layer_head_mask?

    // opt.py: attn_probs.mul_(127).round_()
    // opt.py: attn_probs = attn_probs.to(torch.int8)
    // int8_t attn_probs_int8_arr[this->num_heads * sqlen * sqlen];
    Matrix3D<int8_t> attn_probs_int8(attn_probs_int8_arr, this->num_heads, sqlen, sqlen);
    int len = attn_probs.length();
    for (int i = 0; i < len; i++) attn_probs_int8_arr[i] = static_cast<int8_t>(std::round(attn_probs.m_data[i] * 127));

    // opt.py: value_states = value_states.transpose(1, 2).contiguous()
    int8_t value_states_transpose_arr[this->num_heads * this->head_dim * sqlen];
    Matrix3D<int8_t> value_states_transpose(value_states_transpose_arr, this->num_heads, this->head_dim, sqlen);
    transpose_1_2idx(value_states, value_states_transpose);

    // opt.py: attn_output = self.pv_bmm(attn_probs, value_states)
    int8_t attn_output_arr[this->num_heads * sqlen * this->head_dim];  // TODO: check if tgt_len or sqlen
    Matrix3D<int8_t> attn_output(attn_output_arr, this->num_heads, sqlen, this->head_dim);
    BMM_S8T_S8N_S8T(this->pv_bmm);
    this->pv_bmm.forward(attn_probs_int8, value_states_transpose, attn_output);

    // opt.py: attn_output = attn_output.transpose(1, 2)
    // opt.py: attn_output = attn_output.reshape(bsz, tgt_len, self.embed_dim).contiguous()
    int8_t attn_output_transpose_arr[this->num_heads * sqlen * this->head_dim];  // TODO: check if tgt_len or sqlen
    Matrix3D<int8_t> attn_output_transpose(attn_output_transpose_arr, 1, sqlen, this->num_heads * this->head_dim);
    this->unshape(attn_output, attn_output_transpose, sqlen);

    float attn_output_fp_arr[this->num_heads * sqlen * this->head_dim];  // TODO: check if tgt_len or sqlen
    Matrix3D<float> attn_output_fp(attn_output_fp_arr, 1, sqlen, this->num_heads * this->head_dim);
    this->out_proj.forward(attn_output_transpose, attn_output_fp);
    // output assignment
    output.attn_output = attn_output_fp;
    output.past_key_value = {key_states, value_states};

    return output;
}