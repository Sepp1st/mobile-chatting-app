package com.example.myapplication.data.dao

import androidx.lifecycle.LiveData
import androidx.room.*
import com.example.myapplication.data.model.Message

@Dao
interface MessageDao {
    @Query("SELECT * FROM messages WHERE (senderId = :userId OR recipientId = :userId) AND groupId IS NULL ORDER BY timestamp DESC")
    fun getDirectMessages(userId: Int): LiveData<List<Message>>
    
    @Query("SELECT * FROM messages WHERE ((senderId = :currentUserId AND recipientId = :otherUserId) OR (senderId = :otherUserId AND recipientId = :currentUserId)) AND groupId IS NULL ORDER BY timestamp ASC")
    fun getConversationMessages(currentUserId: Int, otherUserId: Int): LiveData<List<Message>>
    
    @Query("SELECT * FROM messages WHERE groupId = :groupId ORDER BY timestamp ASC")
    fun getGroupMessages(groupId: Int): LiveData<List<Message>>
    
    @Query("SELECT * FROM messages WHERE isSent = 0 OR isDelivered = 0")
    suspend fun getPendingMessages(): List<Message>
    
    @Insert(onConflict = OnConflictStrategy.IGNORE)
    suspend fun insertMessage(message: Message): Long
    
    @Update
    suspend fun updateMessage(message: Message)
    
    @Query("UPDATE messages SET isRead = 1 WHERE recipientId = :currentUserId AND senderId = :otherUserId")
    suspend fun markMessagesAsRead(currentUserId: Int, otherUserId: Int)
    
    @Query("UPDATE messages SET isSent = 1 WHERE id = :messageId")
    suspend fun markAsSent(messageId: Long)
    
    @Query("UPDATE messages SET isDelivered = 1 WHERE id = :messageId")
    suspend fun markAsDelivered(messageId: Long)
    
    @Query("DELETE FROM messages WHERE timestamp < :timestamp")
    suspend fun deleteOldMessages(timestamp: Long)

    @Query("SELECT * FROM messages WHERE senderId = :senderId AND recipientId = :recipientId AND content = :content ORDER BY timestamp DESC LIMIT 1")
    suspend fun findLatestMatchingMessage(senderId: Int, recipientId: Int, content: String): Message?

    @Query("SELECT * FROM messages WHERE senderId = :senderId AND groupId = :groupId AND content = :content ORDER BY timestamp DESC LIMIT 1")
    suspend fun findLatestMatchingGroupMessage(senderId: Int, groupId: Int, content: String): Message?
    
    @Query("DELETE FROM messages WHERE ((senderId = :currentUserId AND recipientId = :otherUserId) OR (senderId = :otherUserId AND recipientId = :currentUserId)) AND groupId IS NULL")
    suspend fun deleteConversation(currentUserId: Int, otherUserId: Int)
    
    @Query("DELETE FROM messages WHERE groupId = :groupId")
    suspend fun deleteGroupMessages(groupId: Int)
}
