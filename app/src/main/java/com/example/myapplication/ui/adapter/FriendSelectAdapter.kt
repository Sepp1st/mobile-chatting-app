package com.example.myapplication.ui.adapter

import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.example.myapplication.data.model.Friend
import com.example.myapplication.databinding.ItemFriendSelectBinding

class FriendSelectAdapter(
    private val onSelectionChanged: (Set<Int>) -> Unit
) : ListAdapter<Friend, FriendSelectAdapter.FriendViewHolder>(FriendDiffCallback()) {

    private val selectedIds = mutableSetOf<Int>()

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): FriendViewHolder {
        val binding = ItemFriendSelectBinding.inflate(
            LayoutInflater.from(parent.context),
            parent,
            false
        )
        return FriendViewHolder(binding)
    }

    override fun onBindViewHolder(holder: FriendViewHolder, position: Int) {
        holder.bind(getItem(position))
    }

    fun getSelectedIds(): Set<Int> = selectedIds.toSet()

    inner class FriendViewHolder(
        private val binding: ItemFriendSelectBinding
    ) : RecyclerView.ViewHolder(binding.root) {

        fun bind(friend: Friend) {
            binding.apply {
                tvName.text = friend.username
                checkbox.isChecked = selectedIds.contains(friend.userId)
                
                root.setOnClickListener {
                    val newCheckedState = !checkbox.isChecked
                    checkbox.isChecked = newCheckedState
                    toggleSelection(friend.userId, newCheckedState)
                }
                
                checkbox.setOnCheckedChangeListener { _, isChecked ->
                    if (isChecked) {
                        selectedIds.add(friend.userId)
                    } else {
                        selectedIds.remove(friend.userId)
                    }
                    onSelectionChanged(selectedIds)
                }
            }
        }
        
        private fun toggleSelection(userId: Int, isSelected: Boolean) {
            if (isSelected) {
                selectedIds.add(userId)
            } else {
                selectedIds.remove(userId)
            }
            onSelectionChanged(selectedIds)
        }
    }

    private class FriendDiffCallback : DiffUtil.ItemCallback<Friend>() {
        override fun areItemsTheSame(oldItem: Friend, newItem: Friend): Boolean {
            return oldItem.userId == newItem.userId
        }

        override fun areContentsTheSame(oldItem: Friend, newItem: Friend): Boolean {
            return oldItem == newItem
        }
    }
}
